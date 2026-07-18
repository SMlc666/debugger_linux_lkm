#include <stdio.h>
#include <unistd.h>

#include "lkmdbg/lkmdbg.h"

int main(void)
{
	struct lkmdbg_status_reply status;
	struct lkmdbg_event_config_request event_config;
	struct lkmdbg_session *session;

	if (lkmdbg_session_open(&session) < 0)
		return 1;

	if (lkmdbg_session_get_status(session, &status) < 0) {
		lkmdbg_session_close(session);
		return 1;
	}
	if (lkmdbg_event_config_get(session, &event_config) < 0) {
		lkmdbg_session_close(session);
		return 1;
	}

	printf("example_session_status: ok target_tgid=%d stealth=0x%x\n",
	       status.target_tgid, status.stealth_flags);
	lkmdbg_session_close(session);
	return 0;
}
