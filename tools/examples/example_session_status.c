#include <stdio.h>
#include <unistd.h>

#include "../driver/bridge_c.h"

int main(void)
{
	struct lkmdbg_status_reply status;
	int session_fd;
	const char *anchor = "none";

	session_fd = open_session_fd();
	if (session_fd < 0)
		return 1;

	if (get_status(session_fd, &status) < 0) {
		close(session_fd);
		return 1;
	}

	if (status.target_tgid > 0 && status.target_tid > 0) {
		anchor = status.target_tid == status.target_tgid ?
				 "process-anchor" :
				 "thread-anchor";
	}

	printf("example_session_status: ok target_tgid=%d target_tid=%d anchor=%s stealth=0x%x\n",
	       status.target_tgid, status.target_tid, anchor, status.stealth_flags);
	close(session_fd);
	return 0;
}
