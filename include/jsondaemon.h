/*
 * jsondaemon plugin API.
 *
 * A plugin is a shared object dropped in the daemon's plugin directory. It
 * exports two functions, and that is enough to become an HTTP/JSON API:
 *
 *   const char *jd_mount(void);
 *       The HTTP path the plugin wants to be mounted at, e.g. "/hello".
 *       Asked once, right after the plugin is loaded and before anything else.
 *       The string is copied: a string literal is perfect.
 *
 *   json_object *jd_call(const char *method, json_object *params, jd_error *err);
 *       Handles one call of `method`. `params` is borrowed (never put it) and is
 *       NULL when the caller sent none. Return a new reference: the daemon takes
 *       ownership of it. Returning NULL without an error is a JSON `null`
 *       result. To fail, `return jd_fail(err, code, "fmt", ...);`.
 *
 * Optional exports:
 *
 *   int jd_init(void);
 *       Called once after the mount is accepted. Non-zero refuses the load.
 *   void jd_fini(void);
 *       Called once before the plugin is unloaded (removed or replaced).
 *   json_object *jd_describe(void);
 *       Self-description shown by `GET /` and `GET /<mount>`. New reference.
 *   const unsigned jd_flags = JD_THREAD_SAFE;
 *       By default the daemon never runs two jd_call() of the same plugin at
 *       the same time, so non-reentrant legacy code is safe. Export this flag
 *       to let calls run concurrently.
 *
 * Build a plugin with:
 *   cc -shared -fPIC $(pkg-config --cflags --libs json-c) -I<this dir> my.c -o my.so
 */
#ifndef JSONDAEMON_H
#define JSONDAEMON_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include <json-c/json.h>

#define JD_API_VERSION 1

/* Error codes defined by JSON-RPC 2.0. Any other code is application defined. */
#define JD_PARSE_ERROR      (-32700)
#define JD_INVALID_REQUEST  (-32600)
#define JD_METHOD_NOT_FOUND (-32601)
#define JD_INVALID_PARAMS   (-32602)
#define JD_INTERNAL_ERROR   (-32603)

/* Bits of jd_flags. */
#define JD_THREAD_SAFE 0x1u

#if defined(__GNUC__) || defined(__clang__)
#define JD_EXPORT __attribute__((visibility("default")))
#define JD_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define JD_EXPORT
#define JD_PRINTF(fmt, args)
#endif

typedef struct jd_error {
	int code;          /* 0: no error */
	char message[256]; /* human readable, NUL terminated */
	json_object *data; /* optional extra details, new reference */
} jd_error;

/* What a plugin exports. Declared here so the compiler checks the signatures. */
JD_EXPORT const char *jd_mount(void);
JD_EXPORT json_object *jd_call(const char *method, json_object *params, jd_error *err);
JD_EXPORT int jd_init(void);
JD_EXPORT void jd_fini(void);
JD_EXPORT json_object *jd_describe(void);
JD_EXPORT extern const unsigned jd_flags;

/* Fills `err` and returns NULL, so a handler can `return jd_fail(...)`. */
static inline json_object *jd_fail(jd_error *err, int code, const char *fmt, ...) JD_PRINTF(3, 4);
static inline json_object *jd_fail(jd_error *err, int code, const char *fmt, ...)
{
	va_list ap;

	err->code = code;
	va_start(ap, fmt);
	vsnprintf(err->message, sizeof(err->message), fmt, ap);
	va_end(ap);
	return NULL;
}

/*
 * Fetches a parameter that can be passed either by position (params is an
 * array) or by name (params is an object). Borrowed reference, NULL if absent.
 */
static inline json_object *jd_param(json_object *params, size_t index, const char *name)
{
	json_object *value = NULL;

	if (params == NULL)
		return NULL;
	if (json_object_is_type(params, json_type_array))
		return index < json_object_array_length(params) ? json_object_array_get_idx(params, index) : NULL;
	if (name != NULL && json_object_is_type(params, json_type_object))
		json_object_object_get_ex(params, name, &value);
	return value;
}

#endif /* JSONDAEMON_H */
