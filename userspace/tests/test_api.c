#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "lkmdbg/lkmdbg.h"

#define CHECK(expr)                                                          \
	do {                                                                   \
		if (!(expr)) {                                                   \
			fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__,    \
				__LINE__, #expr);                                  \
			return 1;                                                  \
		}                                                              \
	} while (0)

int main(void)
{
	struct lkmdbg_session *session = NULL;
	struct lkmdbg_transfer_result result = { 99, 99 };
	uint8_t byte = 0;
	int fd;

	CHECK(LKMDBG_SDK_VERSION_MAJOR == 0);
	errno = 0;
	CHECK(lkmdbg_session_open(NULL) == -1 && errno == EINVAL);
	errno = 0;
	CHECK(lkmdbg_session_adopt_fd(-1, 0, &session) == -1 && errno == EINVAL);

	fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	CHECK(fd >= 0);
	CHECK(lkmdbg_session_adopt_fd(fd, 0, &session) == 0);
	CHECK(lkmdbg_session_fd(session) == fd);

	errno = 0;
	CHECK(lkmdbg_session_set_target(session, 0, 0) == -1 && errno == EINVAL);
	errno = 0;
	CHECK(lkmdbg_memory_read(session, 0, NULL, 1, 0, &result) == -1 &&
	      errno == EINVAL);
	errno = 0;
	CHECK(lkmdbg_memory_read(session, 0, &byte, 0, 0, &result) == -1 &&
	      errno == EINVAL);
	errno = 0;
	CHECK(lkmdbg_memory_readv(session, NULL, 0, &result) == -1 &&
	      errno == EINVAL && result.operations_done == 0 &&
	      result.bytes_done == 0);
	errno = 0;
	CHECK(lkmdbg_event_read(session, NULL, 0, NULL, 0) == -1 &&
	      errno == EINVAL);
	errno = 0;
	CHECK(lkmdbg_remote_map_destroy(NULL) == -1 && errno == EINVAL);
	errno = 0;
	CHECK(lkmdbg_threads_query(session, 0, NULL, 0, NULL) == -1 &&
	      errno == EINVAL);
	errno = 0;
	CHECK(lkmdbg_registers_get(session, 0, NULL) == -1 && errno == EINVAL);
	errno = 0;
	CHECK(lkmdbg_registers_set(session, 0, NULL) == -1 && errno == EINVAL);
	errno = 0;
	CHECK(lkmdbg_remote_alloc_create(session, NULL, NULL) == -1 &&
	      errno == EINVAL);
	errno = 0;
	CHECK(lkmdbg_remote_alloc_query(session, 0, NULL, 0, NULL) == -1 &&
	      errno == EINVAL);
	errno = 0;
	CHECK(lkmdbg_remote_alloc_destroy(NULL) == -1 && errno == EINVAL);

	lkmdbg_session_close(session);
	CHECK(fcntl(fd, F_GETFD) >= 0);
	close(fd);
	printf("lkmdbg_api_test: ok\n");
	return 0;
}
