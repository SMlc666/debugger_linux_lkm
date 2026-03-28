#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../include/lkmdbg_ioctl.h"
#include "../driver/bridge_c.h"
#include "../driver/bridge_control.h"

int main(void)
{
	struct lkmdbg_stealth_request before;
	struct lkmdbg_stealth_request after_set;
	struct lkmdbg_stealth_request after_get;
	int session_fd;

	memset(&before, 0, sizeof(before));
	memset(&after_set, 0, sizeof(after_set));
	memset(&after_get, 0, sizeof(after_get));

	session_fd = open_session_fd();
	if (session_fd < 0)
		return 1;

	if (get_stealth(session_fd, &before) < 0) {
		close(session_fd);
		return 1;
	}
	if (set_stealth(session_fd, before.flags, &after_set) < 0) {
		close(session_fd);
		return 1;
	}
	if (get_stealth(session_fd, &after_get) < 0) {
		close(session_fd);
		return 1;
	}
	if (after_set.flags != before.flags || after_get.flags != before.flags) {
		fprintf(stderr,
			"example_stealth_roundtrip: mismatch before=0x%x set=0x%x get=0x%x\n",
			before.flags, after_set.flags, after_get.flags);
		close(session_fd);
		return 1;
	}

	printf("example_stealth_roundtrip: ok flags=0x%x supported=0x%x\n",
	       before.flags, before.supported_flags);
	close(session_fd);
	return 0;
}

