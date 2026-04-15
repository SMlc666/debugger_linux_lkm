#include "bridge_memory.h"
#include "common.hpp"

#include <errno.h>
#include <sys/ioctl.h>

int xfer_target_memory(int session_fd, struct lkmdbg_mem_op *ops,
		       uint32_t op_count, int write,
		       struct lkmdbg_mem_request *reply_out, int verbose)
{
	struct lkmdbg_mem_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.ops_addr = (uintptr_t)ops,
		.op_count = op_count,
	};
	unsigned long cmd = write ? LKMDBG_IOC_WRITE_MEM : LKMDBG_IOC_READ_MEM;

	if (ioctl(session_fd, cmd, &req) < 0) {
		if (reply_out)
			*reply_out = req;
		lkmdbg_log_errorf("%s failed: %s",
				  write ? "WRITE_MEM" : "READ_MEM",
				  strerror(errno));
		return -1;
	}

	if (verbose)
		printf("%s ops_done=%u bytes_done=%" PRIu64 "\n",
		       write ? "WRITE_MEM" : "READ_MEM", req.ops_done,
		       (uint64_t)req.bytes_done);

	if (reply_out)
		*reply_out = req;

	return 0;
}

int xfer_physical_memory(int session_fd, struct lkmdbg_phys_op *ops,
			 uint32_t op_count, int write,
			 struct lkmdbg_phys_request *reply_out, int verbose)
{
	struct lkmdbg_phys_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.ops_addr = (uintptr_t)ops,
		.op_count = op_count,
	};
	unsigned long cmd = write ? LKMDBG_IOC_WRITE_PHYS : LKMDBG_IOC_READ_PHYS;

	if (ioctl(session_fd, cmd, &req) < 0) {
		if (reply_out)
			*reply_out = req;
		lkmdbg_log_errorf("%s failed: %s",
				  write ? "WRITE_PHYS" : "READ_PHYS",
				  strerror(errno));
		return -1;
	}

	if (verbose)
		printf("%s ops_done=%u bytes_done=%" PRIu64 "\n",
		       write ? "WRITE_PHYS" : "READ_PHYS", req.ops_done,
		       (uint64_t)req.bytes_done);

	if (reply_out)
		*reply_out = req;

	return 0;
}

int read_target_memory(int session_fd, uintptr_t remote_addr, void *buf,
		       size_t len, uint32_t *bytes_done_out, int verbose)
{
	return read_target_memory_flags(session_fd, remote_addr, buf, len, 0,
					bytes_done_out, verbose);
}

int read_target_memory_flags(int session_fd, uintptr_t remote_addr, void *buf,
			     size_t len, uint32_t op_flags,
			     uint32_t *bytes_done_out, int verbose)
{
	struct lkmdbg_mem_op op = {
		.remote_addr = remote_addr,
		.local_addr = (uintptr_t)buf,
		.length = len,
		.flags = op_flags,
	};
	struct lkmdbg_mem_request req;

	if (xfer_target_memory(session_fd, &op, 1, 0, &req, verbose) < 0)
		return -1;

	if (bytes_done_out)
		*bytes_done_out = op.bytes_done;

	return req.ops_done == 1 || !op.bytes_done ? 0 : -1;
}

int write_target_memory(int session_fd, uintptr_t remote_addr, const void *buf,
			size_t len, uint32_t *bytes_done_out, int verbose)
{
	return write_target_memory_flags(session_fd, remote_addr, buf, len, 0,
					 bytes_done_out, verbose);
}

int write_target_memory_flags(int session_fd, uintptr_t remote_addr,
			      const void *buf, size_t len,
			      uint32_t op_flags, uint32_t *bytes_done_out,
			      int verbose)
{
	struct lkmdbg_mem_op op = {
		.remote_addr = remote_addr,
		.local_addr = (uintptr_t)buf,
		.length = len,
		.flags = op_flags,
	};
	struct lkmdbg_mem_request req;

	if (xfer_target_memory(session_fd, &op, 1, 1, &req, verbose) < 0)
		return -1;

	if (bytes_done_out)
		*bytes_done_out = op.bytes_done;

	return req.ops_done == 1 || !op.bytes_done ? 0 : -1;
}

int read_target_memoryv(int session_fd, struct lkmdbg_mem_op *ops,
			uint32_t op_count, uint32_t *ops_done_out,
			uint64_t *bytes_done_out, int verbose)
{
	struct lkmdbg_mem_request req;

	if (xfer_target_memory(session_fd, ops, op_count, 0, &req, verbose) < 0)
		return -1;

	if (ops_done_out)
		*ops_done_out = req.ops_done;
	if (bytes_done_out)
		*bytes_done_out = req.bytes_done;
	return 0;
}

int write_target_memoryv(int session_fd, struct lkmdbg_mem_op *ops,
			 uint32_t op_count, uint32_t *ops_done_out,
			 uint64_t *bytes_done_out, int verbose)
{
	struct lkmdbg_mem_request req;

	if (xfer_target_memory(session_fd, ops, op_count, 1, &req, verbose) < 0)
		return -1;

	if (ops_done_out)
		*ops_done_out = req.ops_done;
	if (bytes_done_out)
		*bytes_done_out = req.bytes_done;
	return 0;
}

int read_physical_memory(int session_fd, uint64_t phys_addr, void *buf,
			 size_t len, uint32_t *bytes_done_out, int verbose)
{
	return read_physical_memory_flags(session_fd, phys_addr, buf, len, 0,
					  bytes_done_out, verbose);
}

int read_physical_memory_flags(int session_fd, uint64_t phys_addr, void *buf,
			       size_t len, uint32_t op_flags,
			       uint32_t *bytes_done_out, int verbose)
{
	struct lkmdbg_phys_op op = {
		.phys_addr = phys_addr,
		.local_addr = (uintptr_t)buf,
		.length = len,
		.flags = op_flags,
	};
	struct lkmdbg_phys_request req;

	if (xfer_physical_memory(session_fd, &op, 1, 0, &req, verbose) < 0)
		return -1;

	if (bytes_done_out)
		*bytes_done_out = op.bytes_done;

	return req.ops_done == 1 || !op.bytes_done ? 0 : -1;
}

int write_physical_memory(int session_fd, uint64_t phys_addr, const void *buf,
			  size_t len, uint32_t *bytes_done_out, int verbose)
{
	return write_physical_memory_flags(session_fd, phys_addr, buf, len, 0,
					   bytes_done_out, verbose);
}

int write_physical_memory_flags(int session_fd, uint64_t phys_addr,
				const void *buf, size_t len,
				uint32_t op_flags, uint32_t *bytes_done_out,
				int verbose)
{
	struct lkmdbg_phys_op op = {
		.phys_addr = phys_addr,
		.local_addr = (uintptr_t)buf,
		.length = len,
		.flags = op_flags,
	};
	struct lkmdbg_phys_request req;

	if (xfer_physical_memory(session_fd, &op, 1, 1, &req, verbose) < 0)
		return -1;

	if (bytes_done_out)
		*bytes_done_out = op.bytes_done;

	return req.ops_done == 1 || !op.bytes_done ? 0 : -1;
}

int create_view_region(int session_fd, uintptr_t base_addr, uint64_t length,
		       uint32_t access_mask, uint32_t backend,
		       uint32_t fault_policy, uint32_t sync_policy,
		       uint32_t writeback_policy,
		       struct lkmdbg_view_region_request *reply_out)
{
	struct lkmdbg_view_region_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.base_addr = base_addr,
		.length = length,
		.access_mask = access_mask,
		.backend = backend,
		.fault_policy = fault_policy,
		.sync_policy = sync_policy,
		.writeback_policy = writeback_policy,
	};

	if (ioctl(session_fd, LKMDBG_IOC_CREATE_VIEW_REGION, &req) < 0) {
		lkmdbg_log_errorf("CREATE_VIEW_REGION failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int set_view_region_backing(int session_fd, uint64_t region_id,
			    uint32_t view_kind, const void *buf,
			    uint64_t length, uint32_t backing_type,
			    struct lkmdbg_view_backing_request *reply_out)
{
	struct lkmdbg_view_backing_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.region_id = region_id,
		.view_kind = view_kind,
		.backing_type = backing_type,
		.source_addr = (uintptr_t)buf,
		.source_length = length,
	};

	if (ioctl(session_fd, LKMDBG_IOC_SET_VIEW_BACKING, &req) < 0) {
		lkmdbg_log_errorf("SET_VIEW_BACKING failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int set_view_region_read_backing(int session_fd, uint64_t region_id,
				 const void *buf, uint64_t length,
				 uint32_t backing_type,
				 struct lkmdbg_view_backing_request *reply_out)
{
	return set_view_region_backing(session_fd, region_id,
				       LKMDBG_VIEW_KIND_READ, buf, length,
				       backing_type, reply_out);
}

int set_view_region_write_backing(int session_fd, uint64_t region_id,
				  const void *buf, uint64_t length,
				  uint32_t backing_type,
				  struct lkmdbg_view_backing_request *reply_out)
{
	return set_view_region_backing(session_fd, region_id,
				       LKMDBG_VIEW_KIND_WRITE, buf, length,
				       backing_type, reply_out);
}

int set_view_region_exec_backing(int session_fd, uint64_t region_id,
				 const void *buf, uint64_t length,
				 uint32_t backing_type,
				 struct lkmdbg_view_backing_request *reply_out)
{
	return set_view_region_backing(session_fd, region_id,
				       LKMDBG_VIEW_KIND_EXEC, buf, length,
				       backing_type, reply_out);
}

int set_view_region_policy(int session_fd, uint64_t region_id, uint32_t backend,
			   uint32_t fault_policy, uint32_t sync_policy,
			   uint32_t writeback_policy,
			   struct lkmdbg_view_policy_request *reply_out)
{
	struct lkmdbg_view_policy_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.region_id = region_id,
		.backend = backend,
		.fault_policy = fault_policy,
		.sync_policy = sync_policy,
		.writeback_policy = writeback_policy,
	};

	if (ioctl(session_fd, LKMDBG_IOC_SET_VIEW_POLICY, &req) < 0) {
		lkmdbg_log_errorf("SET_VIEW_POLICY failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int remove_view_region(int session_fd, uint64_t region_id,
		       struct lkmdbg_view_region_handle_request *reply_out)
{
	struct lkmdbg_view_region_handle_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.region_id = region_id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_REMOVE_VIEW_REGION, &req) < 0) {
		lkmdbg_log_errorf("REMOVE_VIEW_REGION failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int query_view_regions(int session_fd, uint64_t start_id,
		       struct lkmdbg_view_region_entry *entries,
		       uint32_t max_entries,
		       struct lkmdbg_view_region_query_request *reply_out)
{
	struct lkmdbg_view_region_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.entries_addr = (uintptr_t)entries,
		.max_entries = max_entries,
		.start_id = start_id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_VIEW_REGIONS, &req) < 0) {
		lkmdbg_log_errorf("QUERY_VIEW_REGIONS failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int bridge_query_target_vmas_ex(
	int session_fd, uint64_t start_addr, uint32_t flags,
	uint32_t match_flags_mask, uint32_t match_flags_value,
	uint32_t match_prot_mask, uint32_t match_prot_value,
	struct lkmdbg_vma_entry *entries, uint32_t max_entries, char *names,
	uint32_t names_size, struct lkmdbg_vma_query_request *reply_out)
{
	struct lkmdbg_vma_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.start_addr = start_addr,
		.entries_addr = (uintptr_t)entries,
		.max_entries = max_entries,
		.flags = flags,
		.match_flags_mask = match_flags_mask,
		.match_flags_value = match_flags_value,
		.match_prot_mask = match_prot_mask,
		.match_prot_value = match_prot_value,
		.names_addr = (uintptr_t)names,
		.names_size = names_size,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_VMAS, &req) < 0) {
		lkmdbg_log_errorf("QUERY_VMAS failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int bridge_query_target_vmas(
	int session_fd, uint64_t start_addr, struct lkmdbg_vma_entry *entries,
	uint32_t max_entries, char *names, uint32_t names_size,
	struct lkmdbg_vma_query_request *reply_out)
{
	return bridge_query_target_vmas_ex(
		session_fd, start_addr, 0, 0, 0, 0, 0, entries, max_entries,
		names, names_size, reply_out);
}

int bridge_query_target_images(
	int session_fd, uint64_t start_addr, uint32_t flags,
	struct lkmdbg_image_entry *entries, uint32_t max_entries, char *names,
	uint32_t names_size, struct lkmdbg_image_query_request *reply_out)
{
	struct lkmdbg_image_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.start_addr = start_addr,
		.entries_addr = (uintptr_t)entries,
		.max_entries = max_entries,
		.flags = flags,
		.names_addr = (uintptr_t)names,
		.names_size = names_size,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_IMAGES, &req) < 0) {
		lkmdbg_log_errorf("QUERY_IMAGES failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int bridge_query_target_pages_ex(
	int session_fd, uint64_t start_addr, uint64_t length, uint32_t flags,
	struct lkmdbg_page_entry *entries, uint32_t max_entries,
	struct lkmdbg_page_query_request *reply_out)
{
	struct lkmdbg_page_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.start_addr = start_addr,
		.length = length,
		.entries_addr = (uintptr_t)entries,
		.max_entries = max_entries,
		.flags = flags,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_PAGES, &req) < 0) {
		lkmdbg_log_errorf("QUERY_PAGES failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int bridge_query_target_pages(
	int session_fd, uint64_t start_addr, uint64_t length,
	struct lkmdbg_page_entry *entries, uint32_t max_entries,
	struct lkmdbg_page_query_request *reply_out)
{
	return bridge_query_target_pages_ex(session_fd, start_addr, length, 0,
					    entries, max_entries, reply_out);
}

int bridge_apply_pte_patch(int session_fd, uint64_t addr, uint32_t mode,
			   uint32_t flags, uint64_t raw_pte,
			   struct lkmdbg_pte_patch_request *reply_out)
{
	struct lkmdbg_pte_patch_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.addr = addr,
		.raw_pte = raw_pte,
		.mode = mode,
		.flags = flags,
	};

	if (ioctl(session_fd, LKMDBG_IOC_APPLY_PTE_PATCH, &req) < 0) {
		lkmdbg_log_errorf("APPLY_PTE_PATCH failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int bridge_remove_pte_patch(int session_fd, uint64_t id,
			    struct lkmdbg_pte_patch_request *reply_out)
{
	struct lkmdbg_pte_patch_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.id = id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_REMOVE_PTE_PATCH, &req) < 0) {
		lkmdbg_log_errorf("REMOVE_PTE_PATCH failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int bridge_query_pte_patches(
	int session_fd, uint64_t start_id, struct lkmdbg_pte_patch_entry *entries,
	uint32_t max_entries,
	struct lkmdbg_pte_patch_query_request *reply_out)
{
	struct lkmdbg_pte_patch_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.entries_addr = (uintptr_t)entries,
		.max_entries = max_entries,
		.start_id = start_id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_PTE_PATCHES, &req) < 0) {
		lkmdbg_log_errorf("QUERY_PTE_PATCHES failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int bridge_create_remote_map(
	int session_fd, uintptr_t remote_addr, uintptr_t local_addr,
	uint64_t length, uint32_t prot, uint32_t flags, uint32_t timeout_ms,
	struct lkmdbg_remote_map_request *reply_out)
{
	struct lkmdbg_remote_map_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.remote_addr = remote_addr,
		.local_addr = local_addr,
		.length = length,
		.prot = prot,
		.flags = flags,
		.timeout_ms = timeout_ms,
	};

	if (ioctl(session_fd, LKMDBG_IOC_CREATE_REMOTE_MAP, &req) < 0) {
		lkmdbg_log_errorf("CREATE_REMOTE_MAP failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int bridge_remove_remote_map(
	int session_fd, uint64_t map_id,
	struct lkmdbg_remote_map_handle_request *reply_out)
{
	struct lkmdbg_remote_map_handle_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.map_id = map_id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_REMOVE_REMOTE_MAP, &req) < 0) {
		lkmdbg_log_errorf("REMOVE_REMOTE_MAP failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int bridge_query_remote_maps(
	int session_fd, uint64_t start_id, struct lkmdbg_remote_map_entry *entries,
	uint32_t max_entries,
	struct lkmdbg_remote_map_query_request *reply_out)
{
	struct lkmdbg_remote_map_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.entries_addr = (uintptr_t)entries,
		.max_entries = max_entries,
		.start_id = start_id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_REMOTE_MAPS, &req) < 0) {
		lkmdbg_log_errorf("QUERY_REMOTE_MAPS failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int bridge_create_remote_alloc(
	int session_fd, uintptr_t remote_addr, uint64_t length, uint32_t prot,
	uint32_t flags, struct lkmdbg_remote_alloc_request *reply_out)
{
	struct lkmdbg_remote_alloc_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.remote_addr = remote_addr,
		.length = length,
		.prot = prot,
		.flags = flags,
	};

	if (ioctl(session_fd, LKMDBG_IOC_CREATE_REMOTE_ALLOC, &req) < 0) {
		lkmdbg_log_errorf("CREATE_REMOTE_ALLOC failed: %s", strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int bridge_remove_remote_alloc(
	int session_fd, uint64_t alloc_id,
	struct lkmdbg_remote_alloc_handle_request *reply_out)
{
	struct lkmdbg_remote_alloc_handle_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.alloc_id = alloc_id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_REMOVE_REMOTE_ALLOC, &req) < 0) {
		lkmdbg_log_errorf("REMOVE_REMOTE_ALLOC failed: %s",
				  strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}

int bridge_query_remote_allocs(
	int session_fd, uint64_t start_id,
	struct lkmdbg_remote_alloc_entry *entries, uint32_t max_entries,
	struct lkmdbg_remote_alloc_query_request *reply_out)
{
	struct lkmdbg_remote_alloc_query_request req = {
		.version = LKMDBG_PROTO_VERSION,
		.size = sizeof(req),
		.entries_addr = (uintptr_t)entries,
		.max_entries = max_entries,
		.start_id = start_id,
	};

	if (ioctl(session_fd, LKMDBG_IOC_QUERY_REMOTE_ALLOCS, &req) < 0) {
		lkmdbg_log_errorf("QUERY_REMOTE_ALLOCS failed: %s",
				  strerror(errno));
		return -1;
	}

	if (reply_out)
		*reply_out = req;
	return 0;
}
