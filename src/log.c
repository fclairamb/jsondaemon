#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

int jd_log_level = JD_LOG_INFO;

static const char *const level_names[] = {"error", "warn", "info", "debug"};

void jd_log(int level, const char *fmt, ...)
{
	char stamp[32];
	struct timespec now;
	struct tm tm;
	va_list ap;

	if (level > jd_log_level)
		return;
	clock_gettime(CLOCK_REALTIME, &now);
	gmtime_r(&now.tv_sec, &tm);
	strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);

	/* One locked write per line: lines of concurrent connections never mix. */
	flockfile(stderr);
	fprintf(stderr, "%s.%03ldZ %-5s ", stamp, now.tv_nsec / 1000000, level_names[level]);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
	funlockfile(stderr);
}
