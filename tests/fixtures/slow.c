/*
 * Test fixture: a call that takes time and reports how many calls of this
 * plugin were running at once. Built twice, with and without JD_THREAD_SAFE.
 */
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "jsondaemon.h"

#ifdef SLOW_THREAD_SAFE
const unsigned jd_flags = JD_THREAD_SAFE;
#define SLOW_MOUNT "/slow-ts"
#else
#define SLOW_MOUNT "/slow"
#endif

static atomic_int in_flight;

const char *jd_mount(void)
{
	return SLOW_MOUNT;
}

json_object *jd_call(const char *method, json_object *params, jd_error *err)
{
	json_object *ms = jd_param(params, 0, "ms");
	struct timespec ts;
	int64_t delay;
	int seen;

	if (strcmp(method, "sleep") != 0)
		return jd_fail(err, JD_METHOD_NOT_FOUND, "unknown method '%s'", method);
	delay = ms != NULL ? json_object_get_int64(ms) : 100;
	ts.tv_sec = (time_t)(delay / 1000);
	ts.tv_nsec = (long)(delay % 1000) * 1000000L;

	seen = atomic_fetch_add(&in_flight, 1) + 1;
	nanosleep(&ts, NULL);
	{
		int now = atomic_load(&in_flight);

		if (now > seen)
			seen = now;
	}
	atomic_fetch_sub(&in_flight, 1);
	return json_object_new_int(seen);
}
