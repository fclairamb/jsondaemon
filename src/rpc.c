#include "rpc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "registry.h"

#ifndef JD_VERSION
#define JD_VERSION "dev"
#endif

static int is_get(const http_request *req)
{
	return strcmp(req->method, "GET") == 0 || strcmp(req->method, "HEAD") == 0;
}

static int is_post(const http_request *req)
{
	return strcmp(req->method, "POST") == 0;
}

/* Hands a JSON value over as the response body, then releases it. */
static void respond_json(http_response *res, int status, json_object *value)
{
	const char *text = json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);

	res->status = status;
	res->content_type = "application/json";
	res->body_len = strlen(text);
	res->body = malloc(res->body_len + 1);
	if (res->body == NULL)
		res->status = 0;
	else
		memcpy(res->body, text, res->body_len + 1);
	json_object_put(value);
}

static void method_not_allowed(http_response *res, const char *allow)
{
	http_json_error(res, 405, JD_INVALID_REQUEST, "method not allowed");
	res->allow = allow;
}

static int blank(const char *s, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n')
			return 0;
	return 1;
}

/* json-c accepts NaN and Infinity even in strict mode; JSON does not. */
static int has_non_finite(json_object *value)
{
	size_t i, n;

	switch (json_object_get_type(value)) {
	case json_type_double:
		return !isfinite(json_object_get_double(value));
	case json_type_array:
		n = json_object_array_length(value);
		for (i = 0; i < n; i++)
			if (has_non_finite(json_object_array_get_idx(value, i)))
				return 1;
		return 0;
	case json_type_object: {
		json_object_object_foreach(value, key, child) {
			(void)key;
			if (has_non_finite(child))
				return 1;
		}
		return 0;
	}
	default:
		return 0;
	}
}

/* Strict parse of a complete JSON text. Returns 0 and sets *out (may be NULL for `null`). */
static int parse_json(const char *text, size_t len, json_object **out)
{
	json_tokener *tok;
	json_object *value;
	enum json_tokener_error err;
	size_t end;

	*out = NULL;
	if (memchr(text, '\0', len) != NULL || len > 0x7ffffffe)
		return -1;
	tok = json_tokener_new();
	if (tok == NULL)
		return -1;
	json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
	/* Counting the terminating NUL tells json-c the input ends there. */
	value = json_tokener_parse_ex(tok, text, (int)len + 1);
	err = json_tokener_get_error(tok);
	end = json_tokener_get_parse_end(tok);
	json_tokener_free(tok);
	if (err != json_tokener_success || (end < len && !blank(text + end, len - end)) || has_non_finite(value)) {
		json_object_put(value);
		return -1;
	}
	*out = value;
	return 0;
}

/* A JSON-RPC error object. Takes ownership of `data`. */
static json_object *error_object(int code, const char *message, json_object *data)
{
	json_object *error = json_object_new_object();

	json_object_object_add(error, "code", json_object_new_int(code));
	json_object_object_add(error, "message", json_object_new_string(message));
	if (data != NULL)
		json_object_object_add(error, "data", data);
	return error;
}

static const char *error_message(const jd_error *err)
{
	if (err->message[0] != '\0')
		return err->message;
	switch (err->code) {
	case JD_PARSE_ERROR: return "Parse error";
	case JD_INVALID_REQUEST: return "Invalid Request";
	case JD_METHOD_NOT_FOUND: return "Method not found";
	case JD_INVALID_PARAMS: return "Invalid params";
	default: return "Internal error";
	}
}

static json_object *rpc_response(json_object *id, const char *key, json_object *value)
{
	json_object *res = json_object_new_object();

	json_object_object_add(res, "jsonrpc", json_object_new_string("2.0"));
	json_object_object_add(res, key, value);
	json_object_object_add(res, "id", id != NULL ? json_object_get(id) : NULL);
	return res;
}

/* One JSON-RPC request. Returns the response, or NULL for a notification. */
static json_object *rpc_one(plugin *p, json_object *call)
{
	json_object *version, *method, *params = NULL, *id = NULL, *result;
	int has_id = 0;
	jd_error err;

	if (!json_object_is_type(call, json_type_object))
		return rpc_response(NULL, "error", error_object(JD_INVALID_REQUEST, "Invalid Request", NULL));
	if (json_object_object_get_ex(call, "id", &id)) {
		json_type t = json_object_get_type(id);

		has_id = 1;
		if (t != json_type_null && t != json_type_string && t != json_type_int && t != json_type_double)
			return rpc_response(NULL, "error", error_object(JD_INVALID_REQUEST, "Invalid Request: bad id", NULL));
	}
	if (!json_object_object_get_ex(call, "jsonrpc", &version) || !json_object_is_type(version, json_type_string) ||
	    strcmp(json_object_get_string(version), "2.0") != 0)
		return rpc_response(id, "error",
		                    error_object(JD_INVALID_REQUEST, "Invalid Request: jsonrpc must be \"2.0\"", NULL));
	if (!json_object_object_get_ex(call, "method", &method) || !json_object_is_type(method, json_type_string))
		return rpc_response(id, "error",
		                    error_object(JD_INVALID_REQUEST, "Invalid Request: method must be a string", NULL));
	if (json_object_object_get_ex(call, "params", &params) && !json_object_is_type(params, json_type_array) &&
	    !json_object_is_type(params, json_type_object))
		return rpc_response(id, "error",
		                    error_object(JD_INVALID_REQUEST, "Invalid Request: params must be an array or an object",
		                                 NULL));

	result = plugin_call(p, json_object_get_string(method), params, &err);
	if (!has_id) {
		/* A notification: run it, answer nothing. */
		json_object_put(result);
		json_object_put(err.data);
		return NULL;
	}
	if (err.code != 0)
		return rpc_response(id, "error", error_object(err.code, error_message(&err), err.data));
	return rpc_response(id, "result", result);
}

static void handle_jsonrpc(plugin *p, const http_request *req, http_response *res)
{
	json_object *body, *out;

	if (parse_json(req->body, req->body_len, &body) < 0) {
		respond_json(res, 200, rpc_response(NULL, "error", error_object(JD_PARSE_ERROR, "Parse error", NULL)));
		return;
	}
	if (json_object_is_type(body, json_type_array)) {
		size_t i, n = json_object_array_length(body);

		if (n == 0) {
			json_object_put(body);
			respond_json(res, 200,
			             rpc_response(NULL, "error", error_object(JD_INVALID_REQUEST, "Invalid Request: empty batch", NULL)));
			return;
		}
		out = json_object_new_array();
		for (i = 0; i < n; i++) {
			json_object *one = rpc_one(p, json_object_array_get_idx(body, i));

			if (one != NULL)
				json_object_array_add(out, one);
		}
		if (json_object_array_length(out) == 0) {
			json_object_put(out);
			out = NULL;
		}
	} else {
		out = rpc_one(p, body);
	}
	json_object_put(body);
	if (out == NULL) {
		res->status = 204; /* only notifications */
		return;
	}
	respond_json(res, 200, out);
}

/* What an error code becomes on the plain (non JSON-RPC) routes. */
static int status_for(int code)
{
	switch (code) {
	case JD_PARSE_ERROR:
	case JD_INVALID_REQUEST:
	case JD_INVALID_PARAMS:
		return 400;
	case JD_METHOD_NOT_FOUND:
		return 404;
	default:
		/* Internal and implementation-defined server errors vs. application errors. */
		return code == JD_INTERNAL_ERROR || (code >= -32099 && code <= -32000) ? 500 : 422;
	}
}

/*
 * ?a=1&b=two -> {"a":1,"b":"two"}: a value that parses as JSON is taken as
 * such, anything else is a string.
 */
static json_object *query_params(const char *query)
{
	json_object *params;
	char *copy, *pair, *save = NULL;

	if (query == NULL || *query == '\0')
		return NULL;
	copy = strdup(query);
	if (copy == NULL)
		return NULL;
	params = json_object_new_object();
	for (pair = strtok_r(copy, "&", &save); pair != NULL; pair = strtok_r(NULL, "&", &save)) {
		char *eq = strchr(pair, '='), *value = eq != NULL ? eq + 1 : (char *)"";
		json_object *parsed = NULL;

		if (eq != NULL)
			*eq = '\0';
		if (http_url_decode(pair, 1) < 0 || *pair == '\0' || http_url_decode(value, 1) < 0)
			continue;
		if (*value == '\0' || parse_json(value, strlen(value), &parsed) < 0)
			parsed = json_object_new_string(value);
		json_object_object_add(params, pair, parsed);
	}
	free(copy);
	return params;
}

static void handle_call(plugin *p, const char *method, const http_request *req, http_response *res)
{
	json_object *params = NULL, *result;
	jd_error err;

	if (is_post(req)) {
		if (!blank(req->body, req->body_len) && parse_json(req->body, req->body_len, &params) < 0) {
			http_json_error(res, 400, JD_PARSE_ERROR, "request body is not valid JSON");
			return;
		}
	} else if (is_get(req)) {
		params = query_params(req->query);
	} else {
		method_not_allowed(res, "GET, HEAD, POST");
		return;
	}

	result = plugin_call(p, method, params, &err);
	json_object_put(params);
	if (err.code != 0) {
		json_object *body = json_object_new_object();

		json_object_object_add(body, "error", error_object(err.code, error_message(&err), err.data));
		respond_json(res, status_for(err.code), body);
		return;
	}
	if (result == NULL) {
		res->status = 200;
		res->content_type = "application/json";
		res->body = strdup("null");
		res->body_len = res->body != NULL ? 4 : 0;
		return;
	}
	respond_json(res, 200, result);
}

void rpc_handle(const http_request *req, http_response *res, void *ctx)
{
	registry *reg = ctx;
	const char *rest = NULL;
	plugin *p;

	if (strcmp(req->path, "/") == 0) {
		json_object *index;

		if (!is_get(req)) {
			method_not_allowed(res, "GET, HEAD");
			return;
		}
		index = json_object_new_object();
		json_object_object_add(index, "name", json_object_new_string("jsondaemon"));
		json_object_object_add(index, "version", json_object_new_string(JD_VERSION));
		registry_rdlock(reg);
		json_object_object_add(index, "plugins", registry_list(reg));
		registry_unlock(reg);
		respond_json(res, 200, index);
		return;
	}
	if (strcmp(req->path, "/_health") == 0) {
		json_object *health;

		if (!is_get(req)) {
			method_not_allowed(res, "GET, HEAD");
			return;
		}
		health = json_object_new_object();
		json_object_object_add(health, "status", json_object_new_string("ok"));
		registry_rdlock(reg);
		json_object_object_add(health, "plugins", json_object_new_int64((int64_t)registry_count(reg)));
		registry_unlock(reg);
		respond_json(res, 200, health);
		return;
	}

	/* The read lock is held for the whole call: a plugin is never unloaded under it. */
	registry_rdlock(reg);
	p = registry_route(reg, req->path, &rest);
	if (p == NULL) {
		char message[512];

		snprintf(message, sizeof(message), "nothing is mounted at %s", req->path);
		http_json_error(res, 404, JD_METHOD_NOT_FOUND, message);
	} else if (rest[0] == '\0' || strcmp(rest, "/") == 0) {
		if (is_get(req))
			respond_json(res, 200, plugin_info(p));
		else if (is_post(req))
			handle_jsonrpc(p, req, res);
		else
			method_not_allowed(res, "GET, HEAD, POST");
	} else {
		handle_call(p, rest + 1, req, res);
	}
	registry_unlock(reg);
}
