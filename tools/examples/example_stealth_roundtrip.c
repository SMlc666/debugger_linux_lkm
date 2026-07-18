#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lkmdbg/lkmdbg.h"

int main(void)
{
	struct lkmdbg_stealth_request before;
	struct lkmdbg_stealth_request after_set;
	struct lkmdbg_stealth_request after_get;
	struct lkmdbg_session *session;

	memset(&before, 0, sizeof(before));
	memset(&after_set, 0, sizeof(after_set));
	memset(&after_get, 0, sizeof(after_get));

	if (lkmdbg_session_open(&session) < 0)
		return 1;

	if (lkmdbg_stealth_get(session, &before) < 0) {
		lkmdbg_session_close(session);
		return 1;
	}
	if (lkmdbg_stealth_set(session, before.flags, &after_set) < 0) {
		lkmdbg_session_close(session);
		return 1;
	}
	if (lkmdbg_stealth_get(session, &after_get) < 0) {
		lkmdbg_session_close(session);
		return 1;
	}
	if (after_set.flags != before.flags || after_get.flags != before.flags) {
		fprintf(stderr,
			"example_stealth_roundtrip: mismatch before=0x%x set=0x%x get=0x%x\n",
			before.flags, after_set.flags, after_get.flags);
		lkmdbg_session_close(session);
		return 1;
	}

	printf("example_stealth_roundtrip: ok flags=0x%x supported=0x%x\n",
	       before.flags, before.supported_flags);
	lkmdbg_session_close(session);
	return 0;
}
