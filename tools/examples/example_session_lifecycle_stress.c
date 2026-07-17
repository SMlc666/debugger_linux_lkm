#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../driver/bridge_c.h"

#define WORKERS 4
#define ROUNDS 24

struct worker_args {
	unsigned int errors;
};

static void *worker_main(void *opaque)
{
	struct worker_args *args = opaque;

	for (unsigned int round = 0; round < ROUNDS; round++) {
		pid_t child = fork();
		int fd;
		struct lkmdbg_status_reply status;
		int saved;

		if (child < 0) {
			args->errors++;
			continue;
		}
		if (child == 0) {
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000L };
			nanosleep(&ts, NULL);
			_exit(0);
		}

		fd = open_session_fd();
		if (fd < 0) {
			args->errors++;
			waitpid(child, NULL, 0);
			continue;
		}
		if (set_target(fd, child) < 0) {
			saved = errno;
			if (saved != ESRCH && saved != ENOENT)
				args->errors++;
		} else if (get_status(fd, &status) < 0) {
			saved = errno;
			if (saved != ESRCH && saved != ENOENT && saved != ECHILD)
				args->errors++;
		}
		close(fd);
		if (waitpid(child, NULL, 0) < 0 && errno != ECHILD)
			args->errors++;
	}
	return NULL;
}

int main(void)
{
	pthread_t threads[WORKERS];
	struct worker_args args[WORKERS] = { { 0 } };
	unsigned int created = 0;

	for (unsigned int i = 0; i < WORKERS; i++) {
		if (pthread_create(&threads[i], NULL, worker_main, &args[i]) != 0)
			break;
		created++;
	}
	for (unsigned int i = 0; i < created; i++)
		pthread_join(threads[i], NULL);
	if (created != WORKERS)
		return 1;
	for (unsigned int i = 0; i < WORKERS; i++)
		if (args[i].errors)
			return 1;
	printf("example_session_lifecycle_stress: ok workers=%u rounds=%u\n",
	       WORKERS, ROUNDS);
	return 0;
}
