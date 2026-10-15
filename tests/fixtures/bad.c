/*
 * Test fixture: plugins the daemon must refuse, one per build flag.
 */
#include <string.h>

#include "jsondaemon.h"

#ifndef BAD_NO_MOUNT
const char *jd_mount(void)
{
#ifdef BAD_RESERVED
	return "/_admin";
#else
	return "/bad";
#endif
}
#endif

#ifdef BAD_INIT
int jd_init(void)
{
	return 42;
}
#endif

json_object *jd_call(const char *method, json_object *params, jd_error *err)
{
	(void)params;
	return jd_fail(err, JD_METHOD_NOT_FOUND, "unknown method '%s'", method);
}
