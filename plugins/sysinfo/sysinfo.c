/*
 * Wraps plain old libc calls (uname, getloadavg, time): the point of
 * jsondaemon is to turn existing C code into an API without rewriting it.
 * Shows jd_init() / jd_fini() too.
 *
 *   curl localhost:8080/sys/uname
 *   curl localhost:8080/sys/loadavg
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#include "jsondaemon.h"

static time_t loaded_at;

const char *jd_mount(void)
{
	return "/sys";
}

int jd_init(void)
{
	loaded_at = time(NULL);
	return 0;
}

void jd_fini(void)
{
	loaded_at = 0;
}

static json_object *do_uname(jd_error *err)
{
	struct utsname u;
	json_object *o;

	if (uname(&u) < 0)
		return jd_fail(err, JD_INTERNAL_ERROR, "uname() failed");
	o = json_object_new_object();
	json_object_object_add(o, "sysname", json_object_new_string(u.sysname));
	json_object_object_add(o, "release", json_object_new_string(u.release));
	json_object_object_add(o, "version", json_object_new_string(u.version));
	json_object_object_add(o, "machine", json_object_new_string(u.machine));
	return o;
}

static json_object *do_loadavg(jd_error *err)
{
	double load[3];
	json_object *o;
	int i;

	if (getloadavg(load, 3) != 3)
		return jd_fail(err, JD_INTERNAL_ERROR, "getloadavg() failed");
	o = json_object_new_array();
	for (i = 0; i < 3; i++)
		json_object_array_add(o, json_object_new_double(load[i]));
	return o;
}

static json_object *do_time(void)
{
	time_t now = time(NULL);
	char stamp[32];
	struct tm tm;
	json_object *o = json_object_new_object();

	gmtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm);
	json_object_object_add(o, "unix", json_object_new_int64((int64_t)now));
	json_object_object_add(o, "utc", json_object_new_string(stamp));
	json_object_object_add(o, "plugin_uptime", json_object_new_int64((int64_t)(now - loaded_at)));
	return o;
}

json_object *jd_call(const char *method, json_object *params, jd_error *err)
{
	(void)params;
	if (strcmp(method, "uname") == 0)
		return do_uname(err);
	if (strcmp(method, "loadavg") == 0)
		return do_loadavg(err);
	if (strcmp(method, "time") == 0)
		return do_time();
	return jd_fail(err, JD_METHOD_NOT_FOUND, "unknown method '%s', try uname, loadavg or time", method);
}

json_object *jd_describe(void)
{
	json_object *about = json_object_new_object(), *methods = json_object_new_object();

	json_object_object_add(about, "description", json_object_new_string("libc system calls, as an API."));
	json_object_object_add(methods, "uname", json_object_new_string("Operating system name, release and machine."));
	json_object_object_add(methods, "loadavg", json_object_new_string("1, 5 and 15 minutes load averages."));
	json_object_object_add(methods, "time", json_object_new_string("Server time, and since when the plugin is loaded."));
	json_object_object_add(about, "methods", methods);
	return about;
}
