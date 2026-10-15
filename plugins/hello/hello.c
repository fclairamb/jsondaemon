/*
 * The smallest useful plugin: two functions, and it is an HTTP API.
 *
 *   curl localhost:8080/hello/greet?name=Gui
 *   {"message":"Hello, Gui!"}
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsondaemon.h"

#ifndef HELLO_GREETING
#define HELLO_GREETING "Hello"
#endif

const char *jd_mount(void)
{
	return "/hello";
}

json_object *jd_call(const char *method, json_object *params, jd_error *err)
{
	json_object *name, *result;
	const char *who = "world";
	char *message;
	size_t len;

	if (strcmp(method, "greet") != 0)
		return jd_fail(err, JD_METHOD_NOT_FOUND, "unknown method '%s', try 'greet'", method);

	name = jd_param(params, 0, "name");
	if (name != NULL) {
		if (!json_object_is_type(name, json_type_string))
			return jd_fail(err, JD_INVALID_PARAMS, "'name' must be a string");
		who = json_object_get_string(name);
	}

	len = strlen(HELLO_GREETING ", !") + strlen(who) + 1;
	message = malloc(len);
	if (message == NULL)
		return jd_fail(err, JD_INTERNAL_ERROR, "out of memory");
	snprintf(message, len, HELLO_GREETING ", %s!", who);
	result = json_object_new_object();
	json_object_object_add(result, "message", json_object_new_string(message));
	free(message);
	return result;
}
