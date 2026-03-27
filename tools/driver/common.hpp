#ifndef LKMDBG_DRIVER_COMMON_HPP
#define LKMDBG_DRIVER_COMMON_HPP

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LKMDBG_USER_LOG_PREFIX "lkmdbg-user"

static inline void lkmdbg_log_verrorf(const char *fmt, va_list ap)
{
	fprintf(stderr, LKMDBG_USER_LOG_PREFIX ": ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
}

static inline void lkmdbg_log_errorf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	lkmdbg_log_verrorf(fmt, ap);
	va_end(ap);
}

static inline void lkmdbg_log_errnof(const char *op)
{
	lkmdbg_log_errorf("%s failed: %s", op, strerror(errno));
}

static inline int lkmdbg_fprintf(FILE *stream, const char *fmt, ...)
{
	va_list ap;
	int rc;

	va_start(ap, fmt);
	rc = vfprintf(stream, fmt, ap);
	va_end(ap);
	return rc;
}

#endif
