#include <linux/anon_inodes.h>
#include <linux/bitops.h>
#include <linux/file.h>
#include <linux/input.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "lkmdbg_internal.h"

struct lkmdbg_input_device;

struct lkmdbg_input_channel {
	struct list_head session_node;
	struct list_head device_node;
	spinlock_t lock;
	wait_queue_head_t readq;
	struct lkmdbg_session *session;
	struct lkmdbg_input_device *device;
	u64 channel_id;
	u64 device_id;
	u64 event_seq;
	u64 drop_count;
	u32 event_head;
	u32 event_count;
	u32 drop_pending;
	u32 flags;
	bool closing;
	bool disconnected;
	struct lkmdbg_input_event events[LKMDBG_INPUT_EVENT_CAPACITY];
};

struct lkmdbg_input_device {
	struct list_head node;
	spinlock_t lock;
	struct mutex inject_lock;
	struct list_head channels;
	struct input_handle handle;
	u64 device_id;
	u32 flags;
	bool disconnected;
	bool injecting;
	struct task_struct *inject_task;
};

static LIST_HEAD(lkmdbg_input_devices);
static DEFINE_MUTEX(lkmdbg_input_devices_lock);
static u64 lkmdbg_next_input_device_id;

static bool lkmdbg_input_channel_has_events(struct lkmdbg_input_channel *channel)
{
	return READ_ONCE(channel->event_count) > 0;
}

static u32 lkmdbg_input_device_flags_from_dev(struct input_dev *dev)
{
	u32 flags = 0;

	if (!dev)
		return 0;

	if (test_bit(EV_KEY, dev->evbit))
		flags |= LKMDBG_INPUT_DEVICE_FLAG_HAS_KEYS;
	if (test_bit(EV_REL, dev->evbit))
		flags |= LKMDBG_INPUT_DEVICE_FLAG_HAS_REL;
	if (test_bit(EV_ABS, dev->evbit))
		flags |= LKMDBG_INPUT_DEVICE_FLAG_HAS_ABS;
	if (test_bit(ABS_MT_SLOT, dev->absbit) ||
	    test_bit(ABS_MT_TRACKING_ID, dev->absbit) ||
	    test_bit(ABS_MT_POSITION_X, dev->absbit) ||
	    test_bit(ABS_MT_POSITION_Y, dev->absbit))
		flags |= LKMDBG_INPUT_DEVICE_FLAG_HAS_MT;
	if (flags & (LKMDBG_INPUT_DEVICE_FLAG_HAS_KEYS |
		     LKMDBG_INPUT_DEVICE_FLAG_HAS_REL |
		     LKMDBG_INPUT_DEVICE_FLAG_HAS_ABS)) {
		flags |= LKMDBG_INPUT_DEVICE_FLAG_CAN_INJECT;
	}

	return flags;
}

static void lkmdbg_input_fill_device_entry(
	struct lkmdbg_input_device *device,
	struct lkmdbg_input_device_entry *entry)
{
	struct input_dev *dev = device->handle.dev;

	memset(entry, 0, sizeof(*entry));
	entry->device_id = device->device_id;
	entry->bustype = dev->id.bustype;
	entry->vendor = dev->id.vendor;
	entry->product = dev->id.product;
	entry->version_id = dev->id.version;
	entry->flags = device->flags;
	strscpy(entry->name, dev->name ? dev->name : "", sizeof(entry->name));
	strscpy(entry->phys, dev->phys ? dev->phys : "", sizeof(entry->phys));
	strscpy(entry->uniq, dev->uniq ? dev->uniq : "", sizeof(entry->uniq));
}

static void lkmdbg_input_copy_bitmap64(u64 *dst, u32 dst_words,
				       const unsigned long *src,
				       unsigned int bit_count)
{
	unsigned int bit;

	memset(dst, 0, dst_words * sizeof(*dst));
	for (bit = 0; bit < bit_count; bit++) {
		if (!test_bit(bit, src))
			continue;
		dst[bit / 64U] |= 1ULL << (bit % 64U);
	}
}

static void
lkmdbg_input_fill_device_info(struct lkmdbg_input_device *device,
			      struct lkmdbg_input_device_info_request *req)
{
	struct input_dev *dev = device->handle.dev;
	unsigned int i;

	memset(req, 0, sizeof(*req));
	req->version = LKMDBG_PROTO_VERSION;
	req->size = sizeof(*req);
	req->device_id = device->device_id;
	req->flags = device->flags;
	req->supported_channel_flags = LKMDBG_INPUT_CHANNEL_FLAG_INCLUDE_INJECTED;
	lkmdbg_input_fill_device_entry(device, &req->entry);
	lkmdbg_input_copy_bitmap64(req->ev_bits, ARRAY_SIZE(req->ev_bits),
				   dev->evbit, EV_CNT);
	lkmdbg_input_copy_bitmap64(req->key_bits, ARRAY_SIZE(req->key_bits),
				   dev->keybit, KEY_CNT);
	lkmdbg_input_copy_bitmap64(req->rel_bits, ARRAY_SIZE(req->rel_bits),
				   dev->relbit, REL_CNT);
	lkmdbg_input_copy_bitmap64(req->abs_bits, ARRAY_SIZE(req->abs_bits),
				   dev->absbit, ABS_CNT);
	lkmdbg_input_copy_bitmap64(req->prop_bits, ARRAY_SIZE(req->prop_bits),
				   dev->propbit, INPUT_PROP_CNT);

	if (!dev->absinfo)
		return;

	for (i = 0; i < LKMDBG_INPUT_ABS_COUNT && i < ABS_CNT; i++) {
		struct input_absinfo *src = &dev->absinfo[i];

		req->absinfo[i].value = src->value;
		req->absinfo[i].minimum = src->minimum;
		req->absinfo[i].maximum = src->maximum;
		req->absinfo[i].fuzz = src->fuzz;
		req->absinfo[i].flat = src->flat;
		req->absinfo[i].resolution = src->resolution;
	}
}

static void lkmdbg_input_channel_queue_locked(
	struct lkmdbg_input_channel *channel, u32 type, u32 code, s32 value,
	u32 flags)
{
	struct lkmdbg_input_event *event;
	u32 slot;

	slot = (channel->event_head + channel->event_count) %
	       LKMDBG_INPUT_EVENT_CAPACITY;
	if (channel->event_count == LKMDBG_INPUT_EVENT_CAPACITY) {
		channel->drop_count++;
		channel->drop_pending++;
		channel->event_head =
			(channel->event_head + 1) % LKMDBG_INPUT_EVENT_CAPACITY;
		slot = (channel->event_head + channel->event_count - 1) %
		       LKMDBG_INPUT_EVENT_CAPACITY;
	} else {
		channel->event_count++;
	}

	channel->event_seq++;
	event = &channel->events[slot];
	memset(event, 0, sizeof(*event));
	event->seq = channel->event_seq;
	event->timestamp_ns = ktime_get_ns();
	event->type = type;
	event->code = code;
	event->value = value;
	event->flags = flags;
	event->reserved0 = channel->drop_pending;
	channel->drop_pending = 0;
}

static void lkmdbg_input_channel_shutdown(struct lkmdbg_input_channel *channel,
					  bool disconnected)
{
	unsigned long irqflags;

	spin_lock_irqsave(&channel->lock, irqflags);
	channel->closing = true;
	if (disconnected)
		channel->disconnected = true;
	spin_unlock_irqrestore(&channel->lock, irqflags);
	wake_up_interruptible(&channel->readq);
}

static bool lkmdbg_input_channel_detach_from_device(
	struct lkmdbg_input_channel *channel, struct lkmdbg_input_device *device)
{
	bool removed = false;
	unsigned long irqflags;

	spin_lock_irqsave(&device->lock, irqflags);
	if (channel->device == device) {
		list_del_init(&channel->device_node);
		channel->device = NULL;
		removed = true;
	}
	spin_unlock_irqrestore(&device->lock, irqflags);

	return removed;
}

static bool lkmdbg_input_channel_detach_from_session(
	struct lkmdbg_input_channel *channel, struct lkmdbg_session *session)
{
	bool removed = false;

	mutex_lock(&session->lock);
	if (channel->session == session) {
		list_del_init(&channel->session_node);
		channel->session = NULL;
		removed = true;
	}
	mutex_unlock(&session->lock);

	return removed;
}

static int lkmdbg_input_event_supported(struct input_dev *dev, u32 type, u32 code)
{
	switch (type) {
	case EV_SYN:
		return code < SYN_CNT ? 0 : -EINVAL;
	case EV_KEY:
		return code < KEY_CNT && test_bit(code, dev->keybit) ? 0 :
								 -EOPNOTSUPP;
	case EV_REL:
		return code < REL_CNT && test_bit(code, dev->relbit) ? 0 :
								 -EOPNOTSUPP;
	case EV_ABS:
		return code < ABS_CNT && test_bit(code, dev->absbit) ? 0 :
								 -EOPNOTSUPP;
	default:
		return -EOPNOTSUPP;
	}
}

static ssize_t lkmdbg_input_channel_read(struct file *file, char __user *buf,
					 size_t count, loff_t *ppos)
{
	struct lkmdbg_input_channel *channel = file->private_data;
	struct lkmdbg_input_event events[16];
	size_t max_events;
	size_t copied = 0;
	size_t bytes;
	unsigned long irqflags;
	int ret;

	(void)ppos;

	if (!channel)
		return -ENXIO;

	max_events = count / sizeof(events[0]);
	if (!max_events)
		return -EINVAL;
	if (max_events > ARRAY_SIZE(events))
		max_events = ARRAY_SIZE(events);

	for (;;) {
		spin_lock_irqsave(&channel->lock, irqflags);
		if (channel->event_count > 0)
			break;
		if (channel->closing || channel->disconnected) {
			spin_unlock_irqrestore(&channel->lock, irqflags);
			return -ENODEV;
		}
		spin_unlock_irqrestore(&channel->lock, irqflags);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(
			channel->readq,
			lkmdbg_input_channel_has_events(channel) ||
				READ_ONCE(channel->closing) ||
				READ_ONCE(channel->disconnected));
		if (ret)
			return ret;
	}

	while (copied < max_events && channel->event_count > 0) {
		events[copied] = channel->events[channel->event_head];
		channel->event_head =
			(channel->event_head + 1) % LKMDBG_INPUT_EVENT_CAPACITY;
		channel->event_count--;
		copied++;
	}
	spin_unlock_irqrestore(&channel->lock, irqflags);

	bytes = copied * sizeof(events[0]);
	if (copy_to_user(buf, events, bytes))
		return -EFAULT;

	return bytes;
}

static ssize_t lkmdbg_input_channel_write(struct file *file,
					  const char __user *buf, size_t count,
					  loff_t *ppos)
{
	struct lkmdbg_input_channel *channel = file->private_data;
	struct lkmdbg_input_device *device;
	struct lkmdbg_input_event *events;
	unsigned long irqflags;
	size_t event_count;
	size_t i;
	int ret = 0;

	(void)ppos;

	if (!channel)
		return -ENXIO;
	if (!count || count % sizeof(*events))
		return -EINVAL;

	event_count = count / sizeof(*events);
	if (event_count > 256U)
		return -E2BIG;

	events = memdup_user(buf, count);
	if (IS_ERR(events))
		return PTR_ERR(events);

	device = READ_ONCE(channel->device);
	if (!device) {
		kfree(events);
		return -ENODEV;
	}

	mutex_lock(&device->inject_lock);
	spin_lock_irqsave(&device->lock, irqflags);
	if (channel->device != device || channel->closing || channel->disconnected) {
		spin_unlock_irqrestore(&device->lock, irqflags);
		mutex_unlock(&device->inject_lock);
		kfree(events);
		return -ENODEV;
	}
	device->injecting = true;
	device->inject_task = current;
	spin_unlock_irqrestore(&device->lock, irqflags);

	for (i = 0; i < event_count; i++) {
		ret = lkmdbg_input_event_supported(device->handle.dev, events[i].type,
						   events[i].code);
		if (ret)
			break;
		input_inject_event(&device->handle, events[i].type, events[i].code,
				   events[i].value);
	}

	spin_lock_irqsave(&device->lock, irqflags);
	device->injecting = false;
	device->inject_task = NULL;
	spin_unlock_irqrestore(&device->lock, irqflags);
	mutex_unlock(&device->inject_lock);
	kfree(events);

	if (ret)
		return ret;

	return count;
}

static __poll_t lkmdbg_input_channel_poll(struct file *file, poll_table *wait)
{
	struct lkmdbg_input_channel *channel = file->private_data;
	__poll_t mask = 0;
	unsigned long irqflags;

	if (!channel)
		return EPOLLERR;

	poll_wait(file, &channel->readq, wait);

	spin_lock_irqsave(&channel->lock, irqflags);
	if (channel->event_count > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (channel->closing || channel->disconnected)
		mask |= EPOLLERR | EPOLLHUP;
	spin_unlock_irqrestore(&channel->lock, irqflags);

	return mask;
}

static int lkmdbg_input_channel_release(struct inode *inode, struct file *file)
{
	struct lkmdbg_input_channel *channel = file->private_data;
	struct lkmdbg_input_device *device;
	struct lkmdbg_session *session;

	(void)inode;

	if (!channel)
		return 0;

	session = READ_ONCE(channel->session);
	if (session)
		lkmdbg_input_channel_detach_from_session(channel, session);

	device = READ_ONCE(channel->device);
	if (device)
		lkmdbg_input_channel_detach_from_device(channel, device);

	lkmdbg_input_channel_shutdown(channel, false);
	kfree(channel);
	return 0;
}

static const struct file_operations lkmdbg_input_channel_fops = {
	.owner = THIS_MODULE,
	.release = lkmdbg_input_channel_release,
	.read = lkmdbg_input_channel_read,
	.write = lkmdbg_input_channel_write,
	.poll = lkmdbg_input_channel_poll,
	.llseek = noop_llseek,
};

static struct lkmdbg_input_device *
lkmdbg_find_input_device_locked(u64 device_id)
{
	struct lkmdbg_input_device *device;

	list_for_each_entry(device, &lkmdbg_input_devices, node) {
		if (device->device_id == device_id)
			return device;
	}

	return NULL;
}

static int lkmdbg_validate_input_query(struct lkmdbg_input_query_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (req->flags)
		return -EINVAL;
	if (req->max_entries && !req->entries_addr)
		return -EINVAL;
	return 0;
}

long lkmdbg_query_input_devices(struct lkmdbg_session *session,
				void __user *argp)
{
	struct lkmdbg_input_query_request req;
	struct lkmdbg_input_device_entry *entries = NULL;
	struct lkmdbg_input_device *device;
	u32 max_entries;
	u32 filled = 0;
	int ret;

	(void)session;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_input_query(&req);
	if (ret)
		return ret;

	max_entries = req.max_entries;
	if (max_entries > 256U)
		max_entries = 256U;
	if (max_entries) {
		entries = kcalloc(max_entries, sizeof(*entries), GFP_KERNEL);
		if (!entries)
			return -ENOMEM;
	}

	mutex_lock(&lkmdbg_input_devices_lock);
	list_for_each_entry(device, &lkmdbg_input_devices, node) {
		if (device->disconnected)
			continue;
		if (device->device_id < req.start_id)
			continue;
		if (filled >= max_entries)
			break;
		lkmdbg_input_fill_device_entry(device, &entries[filled]);
		filled++;
	}
	if (!max_entries || filled < max_entries) {
		req.done = 1;
		req.next_id = 0;
	} else {
		req.done = 0;
		req.next_id = filled ? entries[filled - 1].device_id + 1 : req.start_id;
	}
	mutex_unlock(&lkmdbg_input_devices_lock);

	req.entries_filled = filled;
	if (filled &&
	    copy_to_user((void __user *)(uintptr_t)req.entries_addr, entries,
			 filled * sizeof(*entries))) {
		kfree(entries);
		return -EFAULT;
	}

	kfree(entries);
	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static int
lkmdbg_validate_input_device_info(struct lkmdbg_input_device_info_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (req->flags)
		return -EINVAL;
	if (!req->device_id)
		return -EINVAL;
	return 0;
}

long lkmdbg_get_input_device_info(struct lkmdbg_session *session,
				  void __user *argp)
{
	struct lkmdbg_input_device_info_request *req;
	struct lkmdbg_input_device *device;
	int ret;

	(void)session;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	if (copy_from_user(req, argp, sizeof(*req))) {
		kfree(req);
		return -EFAULT;
	}

	ret = lkmdbg_validate_input_device_info(req);
	if (ret) {
		kfree(req);
		return ret;
	}

	mutex_lock(&lkmdbg_input_devices_lock);
	device = lkmdbg_find_input_device_locked(req->device_id);
	if (!device || device->disconnected) {
		mutex_unlock(&lkmdbg_input_devices_lock);
		kfree(req);
		return -ENOENT;
	}
	lkmdbg_input_fill_device_info(device, req);
	mutex_unlock(&lkmdbg_input_devices_lock);

	if (copy_to_user(argp, req, sizeof(*req))) {
		kfree(req);
		return -EFAULT;
	}

	kfree(req);
	return 0;
}

static int lkmdbg_validate_input_channel_request(
	struct lkmdbg_input_channel_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;
	if (req->flags & ~LKMDBG_INPUT_CHANNEL_FLAG_INCLUDE_INJECTED)
		return -EINVAL;
	if (!req->device_id)
		return -EINVAL;
	return 0;
}

long lkmdbg_open_input_channel(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_input_channel_request req;
	struct lkmdbg_input_channel *channel;
	struct lkmdbg_input_device *device;
	struct file *file;
	int fd;
	int ret;
	unsigned long irqflags;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_input_channel_request(&req);
	if (ret)
		return ret;

	channel = kzalloc(sizeof(*channel), GFP_KERNEL);
	if (!channel)
		return -ENOMEM;

	INIT_LIST_HEAD(&channel->session_node);
	INIT_LIST_HEAD(&channel->device_node);
	spin_lock_init(&channel->lock);
	init_waitqueue_head(&channel->readq);
	channel->flags = req.flags;
	channel->session = session;
	channel->device_id = req.device_id;

	file = anon_inode_getfile("lkmdbg-input", &lkmdbg_input_channel_fops,
				  channel, O_RDWR | O_CLOEXEC);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		kfree(channel);
		return ret;
	}

	fd = get_unused_fd_flags(O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fput(file);
		kfree(channel);
		return fd;
	}

	mutex_lock(&lkmdbg_input_devices_lock);
	device = lkmdbg_find_input_device_locked(req.device_id);
	if (!device || device->disconnected) {
		mutex_unlock(&lkmdbg_input_devices_lock);
		put_unused_fd(fd);
		fput(file);
		kfree(channel);
		return -ENOENT;
	}
	mutex_lock(&device->inject_lock);
	channel->device = device;
	req.device_flags = device->flags;
	mutex_unlock(&lkmdbg_input_devices_lock);

	mutex_lock(&session->lock);
	session->next_input_channel_id++;
	channel->channel_id = session->next_input_channel_id;
	list_add_tail(&channel->session_node, &session->input_channels);
	mutex_unlock(&session->lock);

	spin_lock_irqsave(&device->lock, irqflags);
	if (channel->device != device) {
		spin_unlock_irqrestore(&device->lock, irqflags);
		lkmdbg_input_channel_detach_from_session(channel, session);
		mutex_unlock(&device->inject_lock);
		put_unused_fd(fd);
		fput(file);
		kfree(channel);
		return -ENODEV;
	}
	list_add_tail(&channel->device_node, &device->channels);
	spin_unlock_irqrestore(&device->lock, irqflags);

	req.channel_fd = fd;
	req.channel_id = channel->channel_id;
	req.supported_flags = LKMDBG_INPUT_CHANNEL_FLAG_INCLUDE_INJECTED;

	if (copy_to_user(argp, &req, sizeof(req))) {
		lkmdbg_input_channel_detach_from_device(channel, device);
		lkmdbg_input_channel_detach_from_session(channel, session);
		mutex_unlock(&device->inject_lock);
		put_unused_fd(fd);
		fput(file);
		kfree(channel);
		return -EFAULT;
	}

	mutex_unlock(&device->inject_lock);
	fd_install(fd, file);
	return 0;
}

void lkmdbg_input_release_session(struct lkmdbg_session *session)
{
	struct lkmdbg_input_channel *channel;
	struct lkmdbg_input_channel *tmp;
	struct lkmdbg_input_device *device;

	mutex_lock(&session->lock);
	list_for_each_entry_safe(channel, tmp, &session->input_channels,
				 session_node) {
		list_del_init(&channel->session_node);
		channel->session = NULL;
		lkmdbg_input_channel_shutdown(channel, false);
		device = channel->device;
		if (device)
			lkmdbg_input_channel_detach_from_device(channel, device);
	}
	mutex_unlock(&session->lock);
}

static void lkmdbg_input_event(struct input_handle *handle, unsigned int type,
			       unsigned int code, int value)
{
	struct lkmdbg_input_device *device =
		container_of(handle, struct lkmdbg_input_device, handle);
	struct lkmdbg_input_channel *channel;
	unsigned long irqflags;
	bool injected;

	spin_lock_irqsave(&device->lock, irqflags);
	injected = device->injecting && device->inject_task == current;
	list_for_each_entry(channel, &device->channels, device_node) {
		unsigned long channel_irqflags;
		u32 flags = 0;

		if (injected) {
			flags |= LKMDBG_INPUT_EVENT_FLAG_INJECTED;
			if (!(channel->flags &
			      LKMDBG_INPUT_CHANNEL_FLAG_INCLUDE_INJECTED))
				continue;
		}

		spin_lock_irqsave(&channel->lock, channel_irqflags);
		if (!channel->closing && !channel->disconnected)
			lkmdbg_input_channel_queue_locked(channel, type, code, value,
							  flags);
		spin_unlock_irqrestore(&channel->lock, channel_irqflags);
		wake_up_interruptible(&channel->readq);
	}
	spin_unlock_irqrestore(&device->lock, irqflags);
}

static int lkmdbg_input_connect(struct input_handler *handler,
				struct input_dev *dev,
				const struct input_device_id *id)
{
	struct lkmdbg_input_device *device;
	int ret;

	(void)id;

	device = kzalloc(sizeof(*device), GFP_KERNEL);
	if (!device)
		return -ENOMEM;

	INIT_LIST_HEAD(&device->node);
	INIT_LIST_HEAD(&device->channels);
	spin_lock_init(&device->lock);
	mutex_init(&device->inject_lock);
	device->flags = lkmdbg_input_device_flags_from_dev(dev);
	device->handle.dev = dev;
	device->handle.name = "lkmdbg-input";
	device->handle.handler = handler;

	ret = input_register_handle(&device->handle);
	if (ret) {
		kfree(device);
		return ret;
	}

	ret = input_open_device(&device->handle);
	if (ret) {
		input_unregister_handle(&device->handle);
		kfree(device);
		return ret;
	}

	mutex_lock(&lkmdbg_input_devices_lock);
	lkmdbg_next_input_device_id++;
	device->device_id = lkmdbg_next_input_device_id;
	list_add_tail(&device->node, &lkmdbg_input_devices);
	mutex_unlock(&lkmdbg_input_devices_lock);

	return 0;
}

static void lkmdbg_input_disconnect(struct input_handle *handle)
{
	struct lkmdbg_input_device *device =
		container_of(handle, struct lkmdbg_input_device, handle);
	struct lkmdbg_input_channel *channel;
	struct lkmdbg_input_channel *tmp;

	mutex_lock(&lkmdbg_input_devices_lock);
	device->disconnected = true;
	mutex_unlock(&lkmdbg_input_devices_lock);

	mutex_lock(&device->inject_lock);

	spin_lock_irq(&device->lock);
	list_for_each_entry_safe(channel, tmp, &device->channels, device_node) {
		list_del_init(&channel->device_node);
		channel->device = NULL;
		lkmdbg_input_channel_shutdown(channel, true);
	}
	spin_unlock_irq(&device->lock);

	input_close_device(handle);
	input_unregister_handle(handle);
	mutex_unlock(&device->inject_lock);
}

static const struct input_device_id lkmdbg_input_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler lkmdbg_input_handler = {
	.event = lkmdbg_input_event,
	.connect = lkmdbg_input_connect,
	.disconnect = lkmdbg_input_disconnect,
	.name = "lkmdbg-input",
	.id_table = lkmdbg_input_ids,
};

int lkmdbg_input_init(void)
{
	return input_register_handler(&lkmdbg_input_handler);
}

void lkmdbg_input_exit(void)
{
	struct lkmdbg_input_device *device;
	struct lkmdbg_input_device *tmp;

	input_unregister_handler(&lkmdbg_input_handler);

	mutex_lock(&lkmdbg_input_devices_lock);
	list_for_each_entry_safe(device, tmp, &lkmdbg_input_devices, node) {
		list_del_init(&device->node);
		kfree(device);
	}
	mutex_unlock(&lkmdbg_input_devices_lock);
}
