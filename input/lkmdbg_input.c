#include <linux/anon_inodes.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/input.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "lkmdbg_internal.h"

struct lkmdbg_input_device;
struct lkmdbg_input_program;
typedef int (*lkmdbg_class_interface_register_fn)(
	struct class_interface *class_intf);
typedef void (*lkmdbg_class_interface_unregister_fn)(
	struct class_interface *class_intf);

struct lkmdbg_input_channel {
	struct list_head session_node;
	struct list_head device_node;
	spinlock_t lock;
	wait_queue_head_t readq;
	struct lkmdbg_session *session;
	struct lkmdbg_input_device *device;
	struct lkmdbg_input_program *program;
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
	bool hook_acquired;
	struct lkmdbg_input_event events[LKMDBG_INPUT_EVENT_CAPACITY];
};

struct lkmdbg_input_device {
	struct list_head node;
	struct list_head retired_node;
	spinlock_t lock;
	struct mutex inject_lock;
	struct list_head channels;
	struct input_dev *input_dev;
	struct device *dev;
	u64 device_id;
	u32 bustype;
	u16 vendor;
	u16 product;
	u16 version_id;
	u32 flags;
	bool disconnected;
	bool injecting;
	bool controller;
	bool dev_ref_held;
	struct lkmdbg_input_program *program;
	struct task_struct *inject_task;
	u64 inject_source_id;
	u64 vm_program_id;
	spinlock_t transform_lock;
	char name[LKMDBG_INPUT_DEVICE_TEXT_MAX];
	char phys[LKMDBG_INPUT_DEVICE_TEXT_MAX];
	char uniq[LKMDBG_INPUT_DEVICE_TEXT_MAX];
};

struct lkmdbg_input_program {
	struct lkmdbg_input_vm_insn *insns;
	u64 id;
	u32 insn_count;
	u32 state_slots;
	s64 state[LKMDBG_INPUT_VM_MAX_STATE];
	u64 executions;
	u64 rewrites;
	u64 drops;
	u64 vm_errors;
};

static LIST_HEAD(lkmdbg_input_devices);
static LIST_HEAD(lkmdbg_input_retired_devices);
static DEFINE_MUTEX(lkmdbg_input_devices_lock);
static u64 lkmdbg_next_input_device_id;
static u64 lkmdbg_next_input_program_id;

static struct class_interface lkmdbg_input_class_interface;
static struct class *lkmdbg_input_class;
static lkmdbg_class_interface_register_fn
	lkmdbg_input_class_interface_register_fn;
static lkmdbg_class_interface_unregister_fn
	lkmdbg_input_class_interface_unregister_fn;
static bool lkmdbg_input_class_registered;
static struct lkmdbg_inline_hook *lkmdbg_input_event_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_input_event_registry;
static void (*lkmdbg_input_event_orig)(struct input_dev *dev, unsigned int type,
				       unsigned int code, int value);
static atomic_t lkmdbg_input_event_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(lkmdbg_input_event_waitq);
static DEFINE_MUTEX(lkmdbg_input_hook_lock);
static unsigned int lkmdbg_input_hook_users;
static bool lkmdbg_input_hook_forced;

static int lkmdbg_input_hook_get(void);
static void lkmdbg_input_hook_put(void);

static bool lkmdbg_input_channel_has_events(struct lkmdbg_input_channel *channel)
{
	return READ_ONCE(channel->event_count) > 0;
}

static bool lkmdbg_input_is_root_device(struct device *dev)
{
	const char *name;

	if (!dev)
		return false;

	name = dev_name(dev);
	if (!name || strncmp(name, "input", 5))
		return false;

	name += 5;
	if (!*name)
		return false;

	while (*name) {
		if (*name < '0' || *name > '9')
			return false;
		name++;
	}

	return true;
}

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

static struct lkmdbg_input_device *
lkmdbg_find_input_device_by_dev_locked(struct input_dev *input_dev)
{
	struct lkmdbg_input_device *device;

	list_for_each_entry(device, &lkmdbg_input_devices, node) {
		if (device->input_dev == input_dev)
			return device;
	}

	return NULL;
}

static struct lkmdbg_input_device *
lkmdbg_find_input_device_by_dev_rcu(struct input_dev *input_dev)
{
	struct lkmdbg_input_device *device;

	list_for_each_entry_rcu(device, &lkmdbg_input_devices, node) {
		if (READ_ONCE(device->input_dev) == input_dev)
			return device;
	}

	return NULL;
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
		     LKMDBG_INPUT_DEVICE_FLAG_HAS_ABS))
		flags |= LKMDBG_INPUT_DEVICE_FLAG_CAN_INJECT;

	return flags;
}

static void lkmdbg_input_snapshot_string(char *dst, size_t dst_size,
					 const char *src)
{
	size_t max_copy;

	if (!dst || !dst_size)
		return;

	memset(dst, 0, dst_size);
	src = READ_ONCE(src);
	if (!src)
		return;

	max_copy = dst_size - 1;
	if (!max_copy)
		return;

	if (copy_from_kernel_nofault(dst, src, max_copy)) {
		memset(dst, 0, dst_size);
		return;
	}

	dst[dst_size - 1] = '\0';
}

static void lkmdbg_input_refresh_device_metadata(
	struct lkmdbg_input_device *device)
{
	struct input_dev *dev;

	if (!device)
		return;

	dev = READ_ONCE(device->input_dev);
	if (!dev)
		return;

	device->bustype = dev->id.bustype;
	device->vendor = dev->id.vendor;
	device->product = dev->id.product;
	device->version_id = dev->id.version;
	device->flags = lkmdbg_input_device_flags_from_dev(dev);
	lkmdbg_input_snapshot_string(device->name, sizeof(device->name),
				     dev->name);
	lkmdbg_input_snapshot_string(device->phys, sizeof(device->phys),
				     dev->phys);
	lkmdbg_input_snapshot_string(device->uniq, sizeof(device->uniq),
				     dev->uniq);
}

static void lkmdbg_input_fill_device_entry(
	struct lkmdbg_input_device *device,
	struct lkmdbg_input_device_entry *entry)
{
	lkmdbg_input_refresh_device_metadata(device);
	memset(entry, 0, sizeof(*entry));
	entry->device_id = device->device_id;
	entry->bustype = device->bustype;
	entry->vendor = device->vendor;
	entry->product = device->product;
	entry->version_id = device->version_id;
	entry->flags = device->flags;
	strscpy(entry->name, device->name, sizeof(entry->name));
	strscpy(entry->phys, device->phys, sizeof(entry->phys));
	strscpy(entry->uniq, device->uniq, sizeof(entry->uniq));
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
	struct input_dev *dev;
	unsigned int i;

	memset(req, 0, sizeof(*req));
	req->version = LKMDBG_PROTO_VERSION;
	req->size = sizeof(*req);
	req->device_id = device->device_id;
	lkmdbg_input_refresh_device_metadata(device);
	dev = READ_ONCE(device->input_dev);
	if (!dev)
		return;

	req->flags = device->flags;
	req->supported_channel_flags = LKMDBG_INPUT_CHANNEL_FLAG_INCLUDE_INJECTED |
		LKMDBG_INPUT_CHANNEL_FLAG_CONTROLLER |
		LKMDBG_INPUT_CHANNEL_FLAG_RAW_EVENTS |
		LKMDBG_INPUT_CHANNEL_FLAG_PRESENTED_EVENTS;
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

struct lkmdbg_input_vm_result {
	u32 type[LKMDBG_INPUT_VM_MAX_OUTPUTS];
	u32 code[LKMDBG_INPUT_VM_MAX_OUTPUTS];
	s32 value[LKMDBG_INPUT_VM_MAX_OUTPUTS];
	u32 flags;
	u32 count;
	bool drop;
};

static int lkmdbg_input_vm_validate(const struct lkmdbg_input_vm_insn *insns,
					    u32 count, u32 state_slots)
{
	u32 i;

	if (!insns || !count || count > LKMDBG_INPUT_VM_MAX_INSNS ||
	    state_slots > LKMDBG_INPUT_VM_MAX_STATE)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		const struct lkmdbg_input_vm_insn *in = &insns[i];

		if (in->dst >= 8 || in->src >= 8)
			return -EINVAL;
		switch (in->opcode) {
		case LKMDBG_INPUT_VM_OP_NOP:
		case LKMDBG_INPUT_VM_OP_LOAD_CTX:
			if (in->imm < 0 || in->imm > LKMDBG_INPUT_VM_CTX_FLAGS)
				return -EINVAL;
			break;
		case LKMDBG_INPUT_VM_OP_LOAD_STATE:
		case LKMDBG_INPUT_VM_OP_STORE_STATE:
			if (in->imm < 0 || in->imm >= state_slots)
				return -EINVAL;
			break;
		case LKMDBG_INPUT_VM_OP_MOV_IMM:
		case LKMDBG_INPUT_VM_OP_MOV_REG:
		case LKMDBG_INPUT_VM_OP_ADD_IMM:
		case LKMDBG_INPUT_VM_OP_CMP_EQ_IMM:
		case LKMDBG_INPUT_VM_OP_SET_TYPE:
		case LKMDBG_INPUT_VM_OP_SET_CODE:
		case LKMDBG_INPUT_VM_OP_SET_VALUE:
		case LKMDBG_INPUT_VM_OP_EMIT:
		case LKMDBG_INPUT_VM_OP_PASS:
		case LKMDBG_INPUT_VM_OP_DROP:
			break;
		case LKMDBG_INPUT_VM_OP_JNZ:
			if (in->offset <= 0 || i + in->offset >= count)
				return -EINVAL;
			break;
		default:
			return -EINVAL;
		}
	}
	return 0;
}

static int lkmdbg_input_vm_exec(struct lkmdbg_input_program *program,
					struct lkmdbg_input_device *device,
					u32 type, u32 code, s32 value, u64 source,
					u32 input_flags,
					struct lkmdbg_input_vm_result *result)
{
	s64 r[8] = { (s64)type, (s64)code, value, (s64)source,
				(s64)device->device_id, input_flags, 0, 0 };
	u32 pc;

	memset(result, 0, sizeof(*result));
	for (pc = 0; pc < program->insn_count; pc++) {
		const struct lkmdbg_input_vm_insn *in = &program->insns[pc];
		u32 out;

		switch (in->opcode) {
		case LKMDBG_INPUT_VM_OP_NOP:
			break;
		case LKMDBG_INPUT_VM_OP_LOAD_CTX:
			if (in->imm < 0 || in->imm > LKMDBG_INPUT_VM_CTX_FLAGS)
				return -EINVAL;
			r[in->dst] = r[in->imm];
			break;
		case LKMDBG_INPUT_VM_OP_LOAD_STATE:
			r[in->dst] = program->state[in->imm];
			break;
		case LKMDBG_INPUT_VM_OP_STORE_STATE:
			program->state[in->imm] = r[in->src];
			break;
		case LKMDBG_INPUT_VM_OP_MOV_IMM:
			r[in->dst] = in->imm;
			break;
		case LKMDBG_INPUT_VM_OP_MOV_REG:
			r[in->dst] = r[in->src];
			break;
		case LKMDBG_INPUT_VM_OP_ADD_IMM:
			r[in->dst] += in->imm;
			break;
		case LKMDBG_INPUT_VM_OP_CMP_EQ_IMM:
			r[in->dst] = r[in->dst] == in->imm;
			break;
		case LKMDBG_INPUT_VM_OP_JNZ:
			if (r[in->dst])
				pc += in->offset;
			break;
		case LKMDBG_INPUT_VM_OP_SET_TYPE:
			r[0] = r[in->src];
			break;
		case LKMDBG_INPUT_VM_OP_SET_CODE:
			r[1] = r[in->src];
			break;
		case LKMDBG_INPUT_VM_OP_SET_VALUE:
			r[2] = r[in->src];
			break;
		case LKMDBG_INPUT_VM_OP_EMIT:
			if (result->count >= LKMDBG_INPUT_VM_MAX_OUTPUTS)
				return -E2BIG;
			out = result->count++;
			result->type[out] = (u32)r[0];
			result->code[out] = (u32)r[1];
			result->value[out] = (s32)r[2];
			result->flags |= LKMDBG_INPUT_EVENT_FLAG_EMITTED;
			break;
		case LKMDBG_INPUT_VM_OP_PASS:
			if (result->count >= LKMDBG_INPUT_VM_MAX_OUTPUTS)
				return -E2BIG;
			out = result->count++;
			result->type[out] = (u32)r[0];
			result->code[out] = (u32)r[1];
			result->value[out] = (s32)r[2];
			if (result->type[out] != type || result->code[out] != code ||
			    result->value[out] != value)
				result->flags |= LKMDBG_INPUT_EVENT_FLAG_REWRITTEN;
			return 0;
		case LKMDBG_INPUT_VM_OP_DROP:
			result->drop = true;
			result->flags |= LKMDBG_INPUT_EVENT_FLAG_DROPPED;
			return 0;
		default:
			return -EINVAL;
		}
	}
	if (!result->count && !result->drop) {
		result->type[0] = (u32)r[0];
		result->code[0] = (u32)r[1];
		result->value[0] = (s32)r[2];
		result->count = 1;
	}
	return 0;
}

static void lkmdbg_input_deliver_event(struct lkmdbg_input_device *device,
				       u32 type, u32 code, s32 value,
				       bool injected, u32 event_flags)
{
	struct lkmdbg_input_channel *channel;
	unsigned long irqflags;

	if (!device)
		return;

	spin_lock_irqsave(&device->lock, irqflags);
	if (device->disconnected) {
		spin_unlock_irqrestore(&device->lock, irqflags);
		return;
	}

	list_for_each_entry(channel, &device->channels, device_node) {
		unsigned long channel_irqflags;
		u32 flags = 0;

		if (injected) {
			flags |= LKMDBG_INPUT_EVENT_FLAG_INJECTED;
			if (!(channel->flags &
			      LKMDBG_INPUT_CHANNEL_FLAG_INCLUDE_INJECTED))
					continue;
		}
		flags |= event_flags;

		spin_lock_irqsave(&channel->lock, channel_irqflags);
		if (!channel->closing && !channel->disconnected)
			lkmdbg_input_channel_queue_locked(channel, type, code, value,
							  flags);
		spin_unlock_irqrestore(&channel->lock, channel_irqflags);
		wake_up_interruptible(&channel->readq);
	}

	spin_unlock_irqrestore(&device->lock, irqflags);
}

/*
 * Newer Android/GKI builds enable KCFI on input_event() call chains. The
 * replacement must stay __nocfi or the virtio keyboard smoke will fault as
 * soon as the first host key reaches the hook.
 */
static void __nocfi lkmdbg_input_event_replacement(struct input_dev *dev,
						   unsigned int type,
						   unsigned int code,
						   int value)
{
	void (*orig)(struct input_dev *dev, unsigned int type, unsigned int code,
		     int value);
	struct lkmdbg_input_device *device;
	struct lkmdbg_input_program *program;
	struct lkmdbg_input_vm_result result;
	u64 source_id = 0;
	bool injected = false;
	unsigned long irqflags;
	u32 i;

	atomic_inc(&lkmdbg_input_event_inflight);

	rcu_read_lock();
	device = lkmdbg_find_input_device_by_dev_rcu(dev);
	if (device) {
		injected = READ_ONCE(device->injecting) &&
				   READ_ONCE(device->inject_task) == current;
		source_id = injected ? READ_ONCE(device->inject_source_id) : 0;
	}
	if (!device) {
		orig = READ_ONCE(lkmdbg_input_event_orig);
		if (orig)
			orig(dev, type, code, value);
		rcu_read_unlock();
		if (atomic_dec_and_test(&lkmdbg_input_event_inflight))
			wake_up_all(&lkmdbg_input_event_waitq);
		return;
	}

	spin_lock_irqsave(&device->transform_lock, irqflags);
	program = device->program;
	if (!program) {
		spin_unlock_irqrestore(&device->transform_lock, irqflags);
		orig = READ_ONCE(lkmdbg_input_event_orig);
		if (orig)
			orig(dev, type, code, value);
		lkmdbg_input_deliver_event(device, type, code, value, injected, 0);
		rcu_read_unlock();
		if (atomic_dec_and_test(&lkmdbg_input_event_inflight))
			wake_up_all(&lkmdbg_input_event_waitq);
		return;
	}
	program->executions++;
	if (lkmdbg_input_vm_exec(program, device, type, code, value, source_id,
					 injected ? LKMDBG_INPUT_EVENT_FLAG_INJECTED : 0,
					 &result)) {
		program->vm_errors++;
			orig = READ_ONCE(lkmdbg_input_event_orig);
			if (orig)
				orig(dev, type, code, value);
			lkmdbg_input_deliver_event(device, type, code, value, injected,
						    LKMDBG_INPUT_EVENT_FLAG_VM_ERROR);
	} else if (result.drop) {
		program->drops++;
		lkmdbg_input_deliver_event(device, type, code, value, injected,
						    result.flags);
	} else {
		orig = READ_ONCE(lkmdbg_input_event_orig);
		for (i = 0; orig && i < result.count; i++) {
			if (lkmdbg_input_event_supported(dev, result.type[i],
							 result.code[i])) {
				program->vm_errors++;
				orig(dev, type, code, value);
				lkmdbg_input_deliver_event(device, type, code, value,
							    injected,
							    LKMDBG_INPUT_EVENT_FLAG_VM_ERROR);
				goto vm_done;
			}
			if (result.flags & LKMDBG_INPUT_EVENT_FLAG_REWRITTEN)
				program->rewrites++;
			orig(dev, result.type[i], result.code[i], result.value[i]);
			lkmdbg_input_deliver_event(device, result.type[i], result.code[i],
							result.value[i], injected, result.flags);
		}
	}
vm_done:
	spin_unlock_irqrestore(&device->transform_lock, irqflags);
	rcu_read_unlock();

	if (atomic_dec_and_test(&lkmdbg_input_event_inflight))
		wake_up_all(&lkmdbg_input_event_waitq);
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
	if (channel->device != device || channel->closing || channel->disconnected ||
	    device->disconnected || !device->input_dev) {
		spin_unlock_irqrestore(&device->lock, irqflags);
		mutex_unlock(&device->inject_lock);
		kfree(events);
		return -ENODEV;
	}
	device->injecting = true;
	device->inject_task = current;
	device->inject_source_id = channel->channel_id;
	spin_unlock_irqrestore(&device->lock, irqflags);

	for (i = 0; i < event_count; i++) {
		ret = lkmdbg_input_event_supported(device->input_dev,
						   events[i].type,
						   events[i].code);
		if (ret)
			break;
		input_event(device->input_dev, events[i].type, events[i].code,
			    events[i].value);
	}

	spin_lock_irqsave(&device->lock, irqflags);
	device->injecting = false;
	device->inject_task = NULL;
	device->inject_source_id = 0;
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

	device = READ_ONCE(channel->device);
	if (device) {
		unsigned long irqflags;

		spin_lock_irqsave(&device->transform_lock, irqflags);
		if ((channel->flags & LKMDBG_INPUT_CHANNEL_FLAG_CONTROLLER) &&
		    device->controller && device->program == channel->program) {
			if (device->program) {
				kfree(device->program->insns);
				kfree(device->program);
			}
			device->program = NULL;
			device->controller = false;
			device->vm_program_id = 0;
		}
		spin_unlock_irqrestore(&device->transform_lock, irqflags);
	}
	channel->program = NULL;
	if (channel->hook_acquired) {
		lkmdbg_input_hook_put();
		channel->hook_acquired = false;
	}

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

static long lkmdbg_input_channel_ioctl(struct file *file, unsigned int cmd,
					       unsigned long arg)
{
	struct lkmdbg_input_channel *channel = file->private_data;
	struct lkmdbg_input_device *device;
	void __user *argp = (void __user *)arg;
	struct lkmdbg_input_vm_load_request req;
	struct lkmdbg_input_vm_info info;
	struct lkmdbg_input_program *program;
	struct lkmdbg_input_vm_insn *insns;
	unsigned long irqflags;
	size_t bytes;
	int ret;

	if (!channel)
		return -ENXIO;
	device = READ_ONCE(channel->device);
	if (!device)
		return -ENODEV;
	if (!(channel->flags & LKMDBG_INPUT_CHANNEL_FLAG_CONTROLLER))
		return -EPERM;

	switch (cmd) {
	case LKMDBG_INPUT_IOC_LOAD_PROGRAM:
		if (copy_from_user(&req, argp, sizeof(req)))
			return -EFAULT;
		if (req.version != LKMDBG_PROTO_VERSION ||
		    req.size != sizeof(req) || !req.insns_addr ||
		    req.insn_count > LKMDBG_INPUT_VM_MAX_INSNS ||
		    req.state_slots > LKMDBG_INPUT_VM_MAX_STATE)
			return -EINVAL;
		bytes = (size_t)req.insn_count * sizeof(*insns);
		insns = memdup_user(u64_to_user_ptr(req.insns_addr), bytes);
		if (IS_ERR(insns))
			return PTR_ERR(insns);
		ret = lkmdbg_input_vm_validate(insns, req.insn_count,
						       req.state_slots);
		if (ret) {
			kfree(insns);
			return ret;
		}
		program = kzalloc(sizeof(*program), GFP_KERNEL);
		if (!program) {
			kfree(insns);
			return -ENOMEM;
		}
		program->insns = insns;
		program->insn_count = req.insn_count;
		program->state_slots = req.state_slots;
		spin_lock_irqsave(&device->transform_lock, irqflags);
		if (device->program && !(req.flags & LKMDBG_INPUT_VM_FLAG_REPLACE)) {
			spin_unlock_irqrestore(&device->transform_lock, irqflags);
			kfree(program->insns);
			kfree(program);
			return -EBUSY;
		}
		if (device->program) {
			kfree(device->program->insns);
			kfree(device->program);
		}
		program->id = ++lkmdbg_next_input_program_id;
		device->program = program;
		device->controller = true;
		device->vm_program_id = program->id;
		channel->program = program;
		spin_unlock_irqrestore(&device->transform_lock, irqflags);
		req.program_id = program->id;
		if (copy_to_user(argp, &req, sizeof(req)))
			return -EFAULT;
		return 0;
	case LKMDBG_INPUT_IOC_UNLOAD_PROGRAM:
		spin_lock_irqsave(&device->transform_lock, irqflags);
		if (device->program != channel->program) {
			spin_unlock_irqrestore(&device->transform_lock, irqflags);
			return -ENOENT;
		}
		kfree(device->program->insns);
		kfree(device->program);
		device->program = NULL;
		device->controller = false;
		device->vm_program_id = 0;
		channel->program = NULL;
		spin_unlock_irqrestore(&device->transform_lock, irqflags);
		return 0;
	case LKMDBG_INPUT_IOC_GET_PROGRAM_INFO:
		memset(&info, 0, sizeof(info));
		info.version = LKMDBG_PROTO_VERSION;
		info.size = sizeof(info);
		spin_lock_irqsave(&device->transform_lock, irqflags);
		if (!device->program) {
			spin_unlock_irqrestore(&device->transform_lock, irqflags);
			return -ENOENT;
		}
		info.program_id = device->program->id;
		info.insn_count = device->program->insn_count;
		info.state_slots = device->program->state_slots;
		info.executions = device->program->executions;
		info.rewrites = device->program->rewrites;
		info.drops = device->program->drops;
		info.vm_errors = device->program->vm_errors;
		spin_unlock_irqrestore(&device->transform_lock, irqflags);
		return copy_to_user(argp, &info, sizeof(info)) ? -EFAULT : 0;
	case LKMDBG_INPUT_IOC_RESET_STATE:
		spin_lock_irqsave(&device->transform_lock, irqflags);
		if (!device->program) {
			spin_unlock_irqrestore(&device->transform_lock, irqflags);
			return -ENOENT;
		}
		memset(device->program->state, 0, sizeof(device->program->state));
		spin_unlock_irqrestore(&device->transform_lock, irqflags);
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations lkmdbg_input_channel_fops = {
	.owner = THIS_MODULE,
	.release = lkmdbg_input_channel_release,
	.read = lkmdbg_input_channel_read,
	.write = lkmdbg_input_channel_write,
	.poll = lkmdbg_input_channel_poll,
	.unlocked_ioctl = lkmdbg_input_channel_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = lkmdbg_input_channel_ioctl,
#endif
	.llseek = noop_llseek,
};

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
	if (req->flags & ~(LKMDBG_INPUT_CHANNEL_FLAG_INCLUDE_INJECTED |
			   LKMDBG_INPUT_CHANNEL_FLAG_CONTROLLER |
			   LKMDBG_INPUT_CHANNEL_FLAG_RAW_EVENTS |
			   LKMDBG_INPUT_CHANNEL_FLAG_PRESENTED_EVENTS))
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
	if (!device || device->disconnected || !device->input_dev) {
		mutex_unlock(&lkmdbg_input_devices_lock);
		put_unused_fd(fd);
		fput(file);
		kfree(channel);
		return -ENOENT;
	}
	mutex_lock(&device->inject_lock);
	channel->device = device;
	req.device_flags = device->flags;
	if (req.flags & LKMDBG_INPUT_CHANNEL_FLAG_CONTROLLER) {
		spin_lock_irqsave(&device->transform_lock, irqflags);
		if (device->controller) {
			spin_unlock_irqrestore(&device->transform_lock, irqflags);
			mutex_unlock(&lkmdbg_input_devices_lock);
			mutex_unlock(&device->inject_lock);
			put_unused_fd(fd);
			fput(file);
			kfree(channel);
			return -EBUSY;
		}
		device->controller = true;
		spin_unlock_irqrestore(&device->transform_lock, irqflags);
	}
	mutex_unlock(&lkmdbg_input_devices_lock);

	mutex_lock(&session->lock);
	session->next_input_channel_id++;
	channel->channel_id = session->next_input_channel_id;
	list_add_tail(&channel->session_node, &session->input_channels);
	mutex_unlock(&session->lock);

	spin_lock_irqsave(&device->lock, irqflags);
	if (channel->device != device || device->disconnected) {
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

	ret = lkmdbg_input_hook_get();
	if (ret) {
		lkmdbg_input_channel_detach_from_device(channel, device);
		lkmdbg_input_channel_detach_from_session(channel, session);
		if (req.flags & LKMDBG_INPUT_CHANNEL_FLAG_CONTROLLER) {
			spin_lock_irqsave(&device->transform_lock, irqflags);
			device->controller = false;
			spin_unlock_irqrestore(&device->transform_lock, irqflags);
		}
		mutex_unlock(&device->inject_lock);
		put_unused_fd(fd);
		fput(file);
		kfree(channel);
		return ret;
	}
	channel->hook_acquired = true;

	req.channel_fd = fd;
	req.channel_id = channel->channel_id;
	req.supported_flags = LKMDBG_INPUT_CHANNEL_FLAG_INCLUDE_INJECTED |
		LKMDBG_INPUT_CHANNEL_FLAG_CONTROLLER |
		LKMDBG_INPUT_CHANNEL_FLAG_RAW_EVENTS |
		LKMDBG_INPUT_CHANNEL_FLAG_PRESENTED_EVENTS;

	if (copy_to_user(argp, &req, sizeof(req))) {
		lkmdbg_input_channel_detach_from_device(channel, device);
		lkmdbg_input_channel_detach_from_session(channel, session);
		if (channel->hook_acquired) {
			lkmdbg_input_hook_put();
			channel->hook_acquired = false;
		}
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

static int lkmdbg_input_register_device(struct device *dev)
{
	struct input_dev *input_dev;
	struct lkmdbg_input_device *device;

	if (!lkmdbg_input_is_root_device(dev))
		return 0;

	input_dev = to_input_dev(dev);

	device = kzalloc(sizeof(*device), GFP_KERNEL);
	if (!device)
		return -ENOMEM;

	INIT_LIST_HEAD(&device->node);
	INIT_LIST_HEAD(&device->retired_node);
	INIT_LIST_HEAD(&device->channels);
	spin_lock_init(&device->lock);
	spin_lock_init(&device->transform_lock);
	mutex_init(&device->inject_lock);
	device->input_dev = input_dev;
	device->dev = dev;
	device->bustype = input_dev->id.bustype;
	device->vendor = input_dev->id.vendor;
	device->product = input_dev->id.product;
	device->version_id = input_dev->id.version;
	device->flags = lkmdbg_input_device_flags_from_dev(input_dev);
	device->program = NULL;
	device->controller = false;
	device->vm_program_id = 0;
	lkmdbg_input_snapshot_string(device->name, sizeof(device->name),
				     input_dev->name);
	lkmdbg_input_snapshot_string(device->phys, sizeof(device->phys),
				     input_dev->phys);
	lkmdbg_input_snapshot_string(device->uniq, sizeof(device->uniq),
				     input_dev->uniq);
	get_device(dev);
	device->dev_ref_held = true;

	mutex_lock(&lkmdbg_input_devices_lock);
	if (lkmdbg_find_input_device_by_dev_locked(input_dev)) {
		mutex_unlock(&lkmdbg_input_devices_lock);
		put_device(dev);
		kfree(device);
		return 0;
	}
	lkmdbg_next_input_device_id++;
	device->device_id = lkmdbg_next_input_device_id;
	list_add_tail_rcu(&device->node, &lkmdbg_input_devices);
	mutex_unlock(&lkmdbg_input_devices_lock);

	return 0;
}

static void lkmdbg_input_unregister_device(struct device *dev)
{
	struct input_dev *input_dev;
	struct lkmdbg_input_device *device;
	struct lkmdbg_input_channel *channel;
	struct lkmdbg_input_channel *tmp;

	if (!lkmdbg_input_is_root_device(dev))
		return;

	input_dev = to_input_dev(dev);

	mutex_lock(&lkmdbg_input_devices_lock);
	device = lkmdbg_find_input_device_by_dev_locked(input_dev);
	if (!device) {
		mutex_unlock(&lkmdbg_input_devices_lock);
		return;
	}
	device->disconnected = true;
	list_del_rcu(&device->node);
	list_add_tail(&device->retired_node, &lkmdbg_input_retired_devices);
	mutex_unlock(&lkmdbg_input_devices_lock);

	mutex_lock(&device->inject_lock);
	spin_lock_irq(&device->lock);
	list_for_each_entry_safe(channel, tmp, &device->channels, device_node) {
		list_del_init(&channel->device_node);
		channel->device = NULL;
		lkmdbg_input_channel_shutdown(channel, true);
	}
	spin_unlock_irq(&device->lock);
	mutex_unlock(&device->inject_lock);

	WRITE_ONCE(device->input_dev, NULL);
	if (device->dev_ref_held) {
		put_device(dev);
		device->dev_ref_held = false;
	}
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
static int lkmdbg_input_add_dev(struct device *dev,
				struct class_interface *class_intf)
{
	(void)class_intf;

	return lkmdbg_input_register_device(dev);
}

static void lkmdbg_input_remove_dev(struct device *dev,
				    struct class_interface *class_intf)
{
	(void)class_intf;

	lkmdbg_input_unregister_device(dev);
}
#else
static int lkmdbg_input_add_dev(struct device *dev)
{
	return lkmdbg_input_register_device(dev);
}

static void lkmdbg_input_remove_dev(struct device *dev)
{
	lkmdbg_input_unregister_device(dev);
}
#endif

static int lkmdbg_install_input_event_hook(void)
{
	void *target;
	void *orig_fn = NULL;
	int ret;

	if (!lkmdbg_symbols.kallsyms_lookup_name)
		return -ENOENT;

	target = (void *)lkmdbg_kallsyms_lookup_name_runtime("input_event");
	if (!target)
		return -ENOENT;

	lkmdbg_input_event_registry = lkmdbg_hook_registry_register(
		"input_event", target, lkmdbg_input_event_replacement);
	if (!lkmdbg_input_event_registry)
		return -ENOMEM;

	ret = lkmdbg_hook_install(target, lkmdbg_input_event_replacement,
				  &lkmdbg_input_event_hook, &orig_fn);
	if (ret) {
		lkmdbg_hook_registry_unregister(lkmdbg_input_event_registry, ret);
		lkmdbg_input_event_registry = NULL;
		return ret;
	}

	lkmdbg_input_event_orig = orig_fn;
	lkmdbg_hook_registry_mark_installed(lkmdbg_input_event_registry, target,
						orig_fn, 0);
	return 0;
}

static int lkmdbg_input_hook_get(void)
{
	int ret = 0;

	mutex_lock(&lkmdbg_input_hook_lock);
	if (!lkmdbg_input_event_hook) {
		ret = lkmdbg_install_input_event_hook();
		if (ret)
			goto out;
	}
	lkmdbg_input_hook_users++;
out:
	mutex_unlock(&lkmdbg_input_hook_lock);
	return ret;
}

static void lkmdbg_input_hook_put(void)
{
	mutex_lock(&lkmdbg_input_hook_lock);
	if (lkmdbg_input_hook_users)
		lkmdbg_input_hook_users--;
	if (!lkmdbg_input_hook_users && !lkmdbg_input_hook_forced &&
		lkmdbg_input_event_hook) {
		if (!lkmdbg_hook_deactivate(lkmdbg_input_event_hook))
			wait_event_timeout(lkmdbg_input_event_waitq,
				atomic_read(&lkmdbg_input_event_inflight) == 0,
				msecs_to_jiffies(1000));
		lkmdbg_hook_destroy(lkmdbg_input_event_hook);
		lkmdbg_input_event_hook = NULL;
		if (lkmdbg_input_event_registry) {
			lkmdbg_hook_registry_unregister(lkmdbg_input_event_registry, 0);
			lkmdbg_input_event_registry = NULL;
		}
		lkmdbg_input_event_orig = NULL;
	}
	mutex_unlock(&lkmdbg_input_hook_lock);
}

static int lkmdbg_input_resolve_class_interface_helpers(void)
{
	unsigned long addr;

	lkmdbg_input_class_interface_register_fn = NULL;
	lkmdbg_input_class_interface_unregister_fn = NULL;

	addr = lkmdbg_lookup_runtime_symbol_any("class_interface_register");
	if (!addr)
		return -ENOENT;
	lkmdbg_input_class_interface_register_fn =
		(lkmdbg_class_interface_register_fn)addr;

	addr = lkmdbg_lookup_runtime_symbol_any("class_interface_unregister");
	if (!addr) {
		lkmdbg_input_class_interface_register_fn = NULL;
		return -ENOENT;
	}
	lkmdbg_input_class_interface_unregister_fn =
		(lkmdbg_class_interface_unregister_fn)addr;

	return 0;
}

static int __nocfi
lkmdbg_input_class_interface_register_runtime(struct class_interface *class_intf)
{
	if (!lkmdbg_input_class_interface_register_fn)
		return -EOPNOTSUPP;

	return lkmdbg_input_class_interface_register_fn(class_intf);
}

static void __nocfi
lkmdbg_input_class_interface_unregister_runtime(
	struct class_interface *class_intf)
{
	if (!lkmdbg_input_class_interface_unregister_fn)
		return;

	lkmdbg_input_class_interface_unregister_fn(class_intf);
}

static void lkmdbg_input_free_device_list(struct list_head *list)
{
	struct lkmdbg_input_device *device;
	struct lkmdbg_input_device *tmp;

	list_for_each_entry_safe(device, tmp, list, retired_node) {
		list_del_init(&device->retired_node);
		if (device->dev_ref_held && device->dev) {
			put_device(device->dev);
			device->dev_ref_held = false;
		}
		kfree(device);
	}
}

static void lkmdbg_input_retire_all_active_locked(void)
{
	struct lkmdbg_input_device *device;
	struct lkmdbg_input_device *tmp;

	list_for_each_entry_safe(device, tmp, &lkmdbg_input_devices, node) {
		list_del_init(&device->node);
		device->disconnected = true;
		list_add_tail(&device->retired_node, &lkmdbg_input_retired_devices);
	}
}

int lkmdbg_input_init(void)
{
	unsigned long class_addr;
	int ret;

	atomic_set(&lkmdbg_input_event_inflight, 0);
	lkmdbg_input_event_hook = NULL;
	lkmdbg_input_event_registry = NULL;
	lkmdbg_input_event_orig = NULL;
	lkmdbg_input_class_registered = false;
	lkmdbg_input_class = NULL;
	lkmdbg_input_hook_users = 0;
	lkmdbg_input_hook_forced = false;

	if (!lkmdbg_symbols.kallsyms_lookup_name)
		return 0;

	class_addr = lkmdbg_kallsyms_lookup_name_runtime("input_class");
	if (!class_addr) {
		lkmdbg_pr_warn("lkmdbg: input_class unavailable, input tracking disabled\n");
		return 0;
	}

	ret = lkmdbg_input_resolve_class_interface_helpers();
	if (ret) {
		lkmdbg_pr_warn("lkmdbg: class_interface helpers unavailable, input tracking disabled ret=%d\n",
			       ret);
		return 0;
	}

	lkmdbg_input_class = (struct class *)class_addr;
	memset(&lkmdbg_input_class_interface, 0,
	       sizeof(lkmdbg_input_class_interface));
	lkmdbg_input_class_interface.class = lkmdbg_input_class;
	lkmdbg_input_class_interface.add_dev = lkmdbg_input_add_dev;
	lkmdbg_input_class_interface.remove_dev = lkmdbg_input_remove_dev;

	ret = lkmdbg_input_class_interface_register_runtime(
		&lkmdbg_input_class_interface);
	if (ret)
		return ret;
	lkmdbg_input_class_registered = true;

	if (!READ_ONCE(enable_input_tracking))
		return 0;

	ret = lkmdbg_install_input_event_hook();
	if (ret) {
		if (lkmdbg_input_class_registered &&
		    lkmdbg_input_class_interface_unregister_fn) {
			lkmdbg_input_class_interface_unregister_runtime(
				&lkmdbg_input_class_interface);
			lkmdbg_input_class_registered = false;
		}
		synchronize_rcu();
		mutex_lock(&lkmdbg_input_devices_lock);
		lkmdbg_input_retire_all_active_locked();
		lkmdbg_input_free_device_list(&lkmdbg_input_retired_devices);
		INIT_LIST_HEAD(&lkmdbg_input_devices);
		INIT_LIST_HEAD(&lkmdbg_input_retired_devices);
		mutex_unlock(&lkmdbg_input_devices_lock);
		lkmdbg_input_class = NULL;
		return ret;
	}
	lkmdbg_input_hook_forced = true;

	return 0;
}

void lkmdbg_input_exit(void)
{
	long remaining = 1;

	mutex_lock(&lkmdbg_input_hook_lock);
	lkmdbg_input_hook_forced = false;
	mutex_unlock(&lkmdbg_input_hook_lock);

	if (lkmdbg_input_event_hook) {
		if (!lkmdbg_hook_deactivate(lkmdbg_input_event_hook)) {
			remaining = wait_event_timeout(
				lkmdbg_input_event_waitq,
				atomic_read(&lkmdbg_input_event_inflight) == 0,
				msecs_to_jiffies(1000));
			if (!remaining)
				pr_warn("lkmdbg: input_event hook drain timed out inflight=%d\n",
					atomic_read(&lkmdbg_input_event_inflight));
		}

		lkmdbg_hook_destroy(lkmdbg_input_event_hook);
		lkmdbg_input_event_hook = NULL;
	}
	if (lkmdbg_input_event_registry) {
		lkmdbg_hook_registry_unregister(lkmdbg_input_event_registry, 0);
		lkmdbg_input_event_registry = NULL;
	}
	lkmdbg_input_event_orig = NULL;

	if (lkmdbg_input_class) {
		if (lkmdbg_input_class_registered &&
		    lkmdbg_input_class_interface_unregister_fn) {
			lkmdbg_input_class_interface_unregister_runtime(
				&lkmdbg_input_class_interface);
			lkmdbg_input_class_registered = false;
		}
		lkmdbg_input_class = NULL;
	}

	synchronize_rcu();

	mutex_lock(&lkmdbg_input_devices_lock);
	lkmdbg_input_retire_all_active_locked();
	lkmdbg_input_free_device_list(&lkmdbg_input_retired_devices);
	INIT_LIST_HEAD(&lkmdbg_input_devices);
	INIT_LIST_HEAD(&lkmdbg_input_retired_devices);
	mutex_unlock(&lkmdbg_input_devices_lock);
}
