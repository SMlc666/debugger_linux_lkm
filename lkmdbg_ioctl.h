#ifndef _LKMDBG_IOCTL_H
#define _LKMDBG_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define LKMDBG_PROTO_VERSION 1
#define LKMDBG_IOC_MAGIC 0xBD

struct lkmdbg_open_session_request {
	__u32 version;
	__u32 size;
	__u32 flags;
	__u32 reserved;
};

struct lkmdbg_status_reply {
	__u32 version;
	__u32 size;
	__u32 hook_requested;
	__u32 hook_active;
	__s32 owner_tgid;
	__u32 reserved0;
	__u64 session_id;
	__u64 active_sessions;
	__u64 load_jiffies;
	__u64 status_reads;
	__u64 bootstrap_ioctl_calls;
	__u64 session_ioctl_calls;
	__u64 session_opened_total;
	__u64 open_successes;
};

#define LKMDBG_IOC_OPEN_SESSION \
	_IOW(LKMDBG_IOC_MAGIC, 0x01, struct lkmdbg_open_session_request)
#define LKMDBG_IOC_GET_STATUS \
	_IOR(LKMDBG_IOC_MAGIC, 0x02, struct lkmdbg_status_reply)
#define LKMDBG_IOC_RESET_SESSION _IO(LKMDBG_IOC_MAGIC, 0x03)

#endif
