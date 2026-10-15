#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

#ifndef JD_VERSION
#define JD_VERSION "dev"
#endif

const char *http_status_text(int status)
{
	switch (status) {
	case 100: return "Continue";
	case 200: return "OK";
	case 204: return "No Content";
	case 400: return "Bad Request";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 411: return "Length Required";
	case 413: return "Content Too Large";
	case 417: return "Expectation Failed";
	case 422: return "Unprocessable Content";
	case 431: return "Request Header Fields Too Large";
	case 500: return "Internal Server Error";
	case 501: return "Not Implemented";
	case 503: return "Service Unavailable";
	case 505: return "HTTP Version Not Supported";
	default: return "Unknown";
	}
}

static int write_all(int fd, struct iovec *iov, int iovcnt)
{
	while (iovcnt > 0) {
		ssize_t n = writev(fd, iov, iovcnt);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		while (iovcnt > 0 && (size_t)n >= iov->iov_len) {
			n -= (ssize_t)iov->iov_len;
			iov++;
			iovcnt--;
		}
		if (iovcnt > 0) {
			iov->iov_base = (char *)iov->iov_base + n;
			iov->iov_len -= (size_t)n;
		}
	}
	return 0;
}

static int send_response(int fd, const http_response *res, int keep_alive, int head_only)
{
	char header[512], date[64];
	struct iovec iov[2];
	time_t now = time(NULL);
	struct tm tm;
	int len, has_body = res->status != 204 && res->status >= 200;

	gmtime_r(&now, &tm);
	strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);
	len = snprintf(header, sizeof(header),
	               "HTTP/1.1 %d %s\r\n"
	               "Server: jsondaemon/" JD_VERSION "\r\n"
	               "Date: %s\r\n"
	               "Connection: %s\r\n",
	               res->status, http_status_text(res->status), date, keep_alive ? "keep-alive" : "close");
	if (has_body)
		len += snprintf(header + len, sizeof(header) - (size_t)len, "Content-Type: %s\r\nContent-Length: %zu\r\n",
		                res->content_type ? res->content_type : "application/json", res->body_len);
	if (res->allow != NULL)
		len += snprintf(header + len, sizeof(header) - (size_t)len, "Allow: %s\r\n", res->allow);
	len += snprintf(header + len, sizeof(header) - (size_t)len, "\r\n");

	iov[0].iov_base = header;
	iov[0].iov_len = (size_t)len;
	iov[1].iov_base = res->body;
	iov[1].iov_len = has_body && !head_only ? res->body_len : 0;
	return write_all(fd, iov, iov[1].iov_len > 0 ? 2 : 1);
}

void http_json_error(http_response *res, int status, int code, const char *message)
{
	char *body;
	size_t cap = strlen(message) * 6 + 64, len;
	const char *p;

	/* Messages may quote user input (a path, a method): escape them. */
	body = malloc(cap);
	if (body == NULL)
		return;
	len = (size_t)snprintf(body, cap, "{\"error\":{\"code\":%d,\"message\":\"", code);
	for (p = message; *p != '\0'; p++) {
		unsigned char c = (unsigned char)*p;

		if (c == '"' || c == '\\') {
			body[len++] = '\\';
			body[len++] = (char)c;
		} else if (c < 0x20) {
			len += (size_t)snprintf(body + len, cap - len, "\\u%04x", c);
		} else {
			body[len++] = (char)c;
		}
	}
	len += (size_t)snprintf(body + len, cap - len, "\"}}");
	free(res->body);
	res->status = status;
	res->content_type = "application/json";
	res->body = body;
	res->body_len = len;
}

void http_reject(int fd, int status, const char *message)
{
	http_response res = {0};

	http_json_error(&res, status, status >= 500 ? -32603 : -32600, message);
	send_response(fd, &res, 0, 0);
	free(res.body);
}

/*
 * After an early error the client may still be sending its body: closing right
 * away would turn our response into a TCP reset. Stop writing, then swallow
 * what is in flight for a moment.
 */
static void lingering_close(int fd)
{
	struct timeval tv = {1, 0};
	char sink[4096];
	size_t total = 0;

	shutdown(fd, SHUT_WR);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	while (total < (4u << 20)) {
		ssize_t n = recv(fd, sink, sizeof(sink), 0);

		if (n <= 0)
			break;
		total += (size_t)n;
	}
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = tolower(c);
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

int http_url_decode(char *s, int plus)
{
	char *out = s;

	for (; *s != '\0'; s++) {
		if (*s == '%') {
			int hi = hexval((unsigned char)s[1]), lo = hi < 0 ? -1 : hexval((unsigned char)s[2]);

			if (hi < 0 || lo < 0 || (hi == 0 && lo == 0))
				return -1;
			*out++ = (char)(hi * 16 + lo);
			s += 2;
		} else if (plus && *s == '+') {
			*out++ = ' ';
		} else {
			*out++ = *s;
		}
	}
	*out = '\0';
	return 0;
}

/* Returns the length of the header block (terminator included), 0 if incomplete. */
static size_t header_end(const char *buf, size_t len)
{
	size_t i;

	for (i = 0; i + 1 < len; i++) {
		if (buf[i] != '\n')
			continue;
		if (buf[i + 1] == '\n')
			return i + 2;
		if (i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n')
			return i + 3;
	}
	return 0;
}

/* Case-insensitive search of `token` in a comma separated header value. */
static int has_token(const char *value, const char *token)
{
	size_t tlen = strlen(token);

	while (*value != '\0') {
		while (*value == ' ' || *value == '\t' || *value == ',')
			value++;
		if (strncasecmp(value, token, tlen) == 0) {
			const char *end = value + tlen;

			while (*end == ' ' || *end == '\t')
				end++;
			if (*end == '\0' || *end == ',')
				return 1;
		}
		while (*value != '\0' && *value != ',')
			value++;
	}
	return 0;
}

struct parsed {
	int status; /* non-zero: reply with this error and close */
	const char *error;
	unsigned long long content_length;
	int has_length;
	int expect_continue;
	int connection_close;
	int connection_keep_alive;
};

static char *trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
		*--end = '\0';
	return s;
}

static void parse_header(struct parsed *ps, char *line)
{
	char *colon = strchr(line, ':'), *name = line, *value;

	if (colon == NULL || colon == line || line[0] == ' ' || line[0] == '\t') {
		ps->status = 400;
		ps->error = "malformed header line";
		return;
	}
	*colon = '\0';
	if (strpbrk(name, " \t") != NULL) {
		ps->status = 400;
		ps->error = "malformed header name";
		return;
	}
	value = trim(colon + 1);

	if (strcasecmp(name, "Content-Length") == 0) {
		unsigned long long n;
		char *end;

		if (*value == '\0' || strspn(value, "0123456789") != strlen(value) || strlen(value) > 18) {
			ps->status = 400;
			ps->error = "invalid Content-Length";
			return;
		}
		n = strtoull(value, &end, 10);
		if (ps->has_length && n != ps->content_length) {
			ps->status = 400;
			ps->error = "conflicting Content-Length headers";
			return;
		}
		ps->content_length = n;
		ps->has_length = 1;
	} else if (strcasecmp(name, "Transfer-Encoding") == 0) {
		ps->status = 501;
		ps->error = "Transfer-Encoding is not supported, send a Content-Length";
	} else if (strcasecmp(name, "Connection") == 0) {
		if (has_token(value, "close"))
			ps->connection_close = 1;
		if (has_token(value, "keep-alive"))
			ps->connection_keep_alive = 1;
	} else if (strcasecmp(name, "Expect") == 0) {
		if (strcasecmp(value, "100-continue") == 0) {
			ps->expect_continue = 1;
		} else {
			ps->status = 417;
			ps->error = "unsupported expectation";
		}
	}
}

/* Parses the request line and headers of `head` (NUL terminated, modified). */
static void parse_head(http_request *req, struct parsed *ps, char *head)
{
	char *line, *next, *target, *version, *q;

	/* Robustness: ignore empty lines before the request line (RFC 9112 2.2). */
	while (*head == '\r' || *head == '\n')
		head++;
	next = strchr(head, '\n');
	if (next != NULL)
		*next++ = '\0';
	line = trim(head);

	target = strchr(line, ' ');
	version = target ? strchr(target + 1, ' ') : NULL;
	if (target == NULL || version == NULL || (size_t)(target - line) >= sizeof(req->method) || target == line) {
		ps->status = 400;
		ps->error = "malformed request line";
		return;
	}
	*target++ = '\0';
	*version++ = '\0';
	memcpy(req->method, line, (size_t)(target - line));
	if (strncmp(version, "HTTP/1.", 7) != 0 || !isdigit((unsigned char)version[7]) || version[8] != '\0') {
		ps->status = 505;
		ps->error = "only HTTP/1.x is supported";
		return;
	}
	req->minor = version[7] - '0';
	if (target[0] != '/') {
		ps->status = 400;
		ps->error = "request target must be an absolute path";
		return;
	}
	q = strchr(target, '?');
	if (q != NULL) {
		*q++ = '\0';
		req->query = strdup(q);
	}
	req->path = strdup(target);
	if (req->path == NULL || http_url_decode(req->path, 0) < 0) {
		ps->status = 400;
		ps->error = "invalid percent-encoding in path";
		return;
	}

	for (line = next; line != NULL && *line != '\0' && ps->status == 0; line = next) {
		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';
		if (line[0] == '\r' || line[0] == '\0')
			break;
		parse_header(ps, line);
	}
}

static void request_reset(http_request *req)
{
	free(req->path);
	free(req->query);
	free(req->body);
	memset(req, 0, sizeof(*req));
}

static double elapsed_ms(const struct timespec *start)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - start->tv_sec) * 1e3 + (double)(now.tv_nsec - start->tv_nsec) / 1e6;
}

void http_serve(int fd, const http_options *opts, http_handler handler, void *ctx)
{
	char *buf = malloc(HTTP_HEADER_MAX + 1);
	size_t have = 0;
	http_request req = {0};

	if (buf == NULL)
		return;

	for (;;) {
		struct parsed ps = {0};
		http_response res = {0};
		struct timespec start;
		size_t hlen, from_buf;
		int keep_alive, head_only;

		/* 1. Read until the end of the header block. */
		while ((hlen = header_end(buf, have)) == 0) {
			ssize_t n;

			if (have == HTTP_HEADER_MAX) {
				http_reject(fd, 431, "request headers exceed 16384 bytes");
				lingering_close(fd);
				goto out;
			}
			n = recv(fd, buf + have, HTTP_HEADER_MAX - have, 0);
			if (n == 0)
				goto out;
			if (n < 0) {
				if (errno == EINTR)
					continue;
				goto out; /* timeout, reset... */
			}
			have += (size_t)n;
		}
		clock_gettime(CLOCK_MONOTONIC, &start);

		/* 2. Parse it. The header block is consumed, the rest stays in buf. */
		{
			char *head = malloc(hlen + 1);

			if (head == NULL)
				goto out;
			memcpy(head, buf, hlen);
			head[hlen] = '\0';
			parse_head(&req, &ps, head);
			free(head);
		}
		memmove(buf, buf + hlen, have - hlen);
		have -= hlen;

		if (ps.status == 0 && ps.content_length > opts->max_body) {
			ps.status = 413;
			ps.error = "request body is too large";
		}
		if (ps.status != 0) {
			http_reject(fd, ps.status, ps.error);
			log_debug("%s %s -> %d (%s)", req.method[0] ? req.method : "?", req.path ? req.path : "?", ps.status,
			          ps.error);
			lingering_close(fd);
			goto out;
		}
		keep_alive = req.minor >= 1 ? !ps.connection_close : ps.connection_keep_alive;
		req.keep_alive = keep_alive;

		/* 3. Read the body: what is already buffered, then the socket. */
		req.body_len = (size_t)ps.content_length;
		req.body = malloc(req.body_len + 1);
		if (req.body == NULL)
			goto out;
		from_buf = have < req.body_len ? have : req.body_len;
		memcpy(req.body, buf, from_buf);
		memmove(buf, buf + from_buf, have - from_buf);
		have -= from_buf;
		if (from_buf < req.body_len && ps.expect_continue && req.minor >= 1) {
			static char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
			struct iovec iov = {cont, sizeof(cont) - 1};

			if (write_all(fd, &iov, 1) < 0)
				goto out;
		}
		while (from_buf < req.body_len) {
			ssize_t n = recv(fd, req.body + from_buf, req.body_len - from_buf, 0);

			if (n == 0)
				goto out;
			if (n < 0) {
				if (errno == EINTR)
					continue;
				goto out;
			}
			from_buf += (size_t)n;
		}
		req.body[req.body_len] = '\0';

		/* 4. Handle and answer. */
		head_only = strcmp(req.method, "HEAD") == 0;
		handler(&req, &res, ctx);
		if (res.status == 0)
			http_json_error(&res, 500, -32603, "handler produced no response");
		if (send_response(fd, &res, keep_alive, head_only) < 0)
			keep_alive = 0;
		log_debug("%s %s -> %d (%zu bytes, %.2f ms)", req.method, req.path, res.status, res.body_len,
		          elapsed_ms(&start));
		free(res.body);
		request_reset(&req);
		if (!keep_alive)
			break;
	}
out:
	request_reset(&req);
	free(buf);
}
