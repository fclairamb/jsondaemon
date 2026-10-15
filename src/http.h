/*
 * A deliberately small HTTP/1.1 server side: one blocking connection per
 * thread, Content-Length bodies, keep-alive, pipelining, Expect: 100-continue.
 */
#ifndef JD_HTTP_H
#define JD_HTTP_H

#include <stddef.h>

#define HTTP_HEADER_MAX 16384

typedef struct http_request {
	char method[16];
	char *path;  /* percent-decoded, query string removed */
	char *query; /* raw query string without '?', NULL when absent */
	int minor;   /* HTTP/1.<minor> */
	int keep_alive;
	char *body; /* NUL terminated (the NUL is not counted in body_len) */
	size_t body_len;
} http_request;

typedef struct http_response {
	int status;
	const char *content_type;
	const char *allow; /* Allow header, for 405 */
	char *body;        /* malloc'd, freed by the server */
	size_t body_len;
} http_response;

typedef void (*http_handler)(const http_request *req, http_response *res, void *ctx);

typedef struct http_options {
	size_t max_body;
} http_options;

/* Serves requests on `fd` until the peer closes, times out or errs. */
void http_serve(int fd, const http_options *opts, http_handler handler, void *ctx);

/* Writes a one-shot JSON error response, used when no thread can serve. */
void http_reject(int fd, int status, const char *message);

const char *http_status_text(int status);

/* Sets res to a JSON `{"error":{"code":..,"message":..}}` body. */
void http_json_error(http_response *res, int status, int code, const char *message);

/* Decodes %XX (and '+' as space when `plus` is set) in place. -1 on bad input. */
int http_url_decode(char *s, int plus);

#endif /* JD_HTTP_H */
