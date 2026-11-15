/*
 * jsondaemon: drop a shared object in a directory, it becomes an HTTP/JSON API.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "log.h"
#include "registry.h"
#include "rpc.h"

#ifndef JD_VERSION
#define JD_VERSION "dev"
#endif

#define IO_TIMEOUT_SEC 30

typedef struct server {
	registry reg;
	http_options http;
	int listen_fd;
	int max_conns;
	int *conn_fds; /* sockets being served, -1 for a free slot */
	int active;
	pthread_mutex_t mu;
	pthread_cond_t idle;
} server;

typedef struct conn {
	server *srv;
	int fd;
	int slot;
} conn;

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_rescan;

static void on_signal(int sig)
{
	if (sig == SIGHUP)
		g_rescan = 1;
	else
		g_stop = 1;
}

static void usage(FILE *out)
{
	fprintf(out,
	        "Usage: jsondaemon [options]\n"
	        "\n"
	        "Serves every plugin (*.so, *.dylib) of a directory as an HTTP/JSON API,\n"
	        "loading, reloading and unloading them as files come and go.\n"
	        "\n"
	        "  -d, --plugins DIR          plugin directory (default: ./plugins)\n"
	        "  -l, --listen ADDR          address to bind (default: 127.0.0.1)\n"
	        "  -p, --port PORT            port, 0 for any free one (default: 8080)\n"
	        "  -i, --scan-interval MS     directory scan period, 0 to only scan at start\n"
	        "                             and on SIGHUP (default: 1000)\n"
	        "  -b, --max-body BYTES       largest accepted request body (default: 1048576)\n"
	        "  -c, --max-connections N    concurrent connections (default: 128)\n"
	        "  -v, --verbose              log every request\n"
	        "  -q, --quiet                only log warnings and errors\n"
	        "  -V, --version              print the version\n"
	        "  -h, --help                 print this help\n");
}

static long parse_number(const char *arg, const char *what, long min, long max)
{
	char *end;
	long value;

	errno = 0;
	value = strtol(arg, &end, 10);
	if (errno != 0 || *arg == '\0' || *end != '\0' || value < min || value > max) {
		fprintf(stderr, "jsondaemon: invalid %s: %s\n", what, arg);
		exit(2);
	}
	return value;
}

static int open_listener(const char *addr, int port, char *where, size_t where_len)
{
	struct addrinfo hints = {0}, *res, *ai;
	char service[16], host[NI_MAXHOST], serv[NI_MAXSERV];
	struct sockaddr_storage bound;
	socklen_t bound_len = sizeof(bound);
	int fd = -1, one = 1, rc;

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
	snprintf(service, sizeof(service), "%d", port);
	rc = getaddrinfo(addr, service, &hints, &res);
	if (rc != 0) {
		log_error("cannot resolve %s: %s", addr, gai_strerror(rc));
		return -1;
	}
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 128) == 0)
			break;
		rc = errno;
		close(fd);
		fd = -1;
		errno = rc;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		log_error("cannot listen on %s:%d: %s", addr, port, strerror(errno));
		return -1;
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

	getsockname(fd, (struct sockaddr *)&bound, &bound_len);
	getnameinfo((struct sockaddr *)&bound, bound_len, host, sizeof(host), serv, sizeof(serv),
	            NI_NUMERICHOST | NI_NUMERICSERV);
	if (bound.ss_family == AF_INET6)
		snprintf(where, where_len, "http://[%s]:%s", host, serv);
	else
		snprintf(where, where_len, "http://%s:%s", host, serv);
	return fd;
}

static void *conn_main(void *arg)
{
	conn *c = arg;
	server *srv = c->srv;
	struct timeval tv = {IO_TIMEOUT_SEC, 0};
	int one = 1;

	setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
	setsockopt(c->fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	http_serve(c->fd, &srv->http, rpc_handle, &srv->reg);

	/* Leave the slot before closing, so shutdown never targets a reused fd. */
	pthread_mutex_lock(&srv->mu);
	srv->conn_fds[c->slot] = -1;
	pthread_mutex_unlock(&srv->mu);
	close(c->fd);
	pthread_mutex_lock(&srv->mu);
	if (--srv->active == 0)
		pthread_cond_broadcast(&srv->idle);
	pthread_mutex_unlock(&srv->mu);
	free(c);
	return NULL;
}

static void accept_one(server *srv)
{
	pthread_attr_t attr;
	sigset_t all, old;
	pthread_t thread;
	conn *c;
	int fd, slot;

	fd = accept(srv->listen_fd, NULL, NULL);
	if (fd < 0)
		return; /* EAGAIN, ECONNABORTED, EMFILE...: poll again */
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	/* BSD sockets inherit O_NONBLOCK from the listener: connections are blocking. */
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);

	pthread_mutex_lock(&srv->mu);
	for (slot = 0; slot < srv->max_conns && srv->conn_fds[slot] >= 0; slot++)
		;
	if (slot == srv->max_conns) {
		pthread_mutex_unlock(&srv->mu);
		log_warn("too many connections (%d), refusing one", srv->max_conns);
		http_reject(fd, 503, "too many connections, retry later");
		close(fd);
		return;
	}
	srv->conn_fds[slot] = fd;
	srv->active++;
	pthread_mutex_unlock(&srv->mu);

	c = malloc(sizeof(*c));
	if (c != NULL) {
		c->srv = srv;
		c->fd = fd;
		c->slot = slot;
		/* Signals are for the main thread, whose poll() they interrupt. */
		sigfillset(&all);
		pthread_sigmask(SIG_SETMASK, &all, &old);
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&thread, &attr, conn_main, c) == 0) {
			pthread_attr_destroy(&attr);
			pthread_sigmask(SIG_SETMASK, &old, NULL);
			return;
		}
		pthread_attr_destroy(&attr);
		pthread_sigmask(SIG_SETMASK, &old, NULL);
		free(c);
	}
	log_error("cannot start a connection thread");
	pthread_mutex_lock(&srv->mu);
	srv->conn_fds[slot] = -1;
	srv->active--;
	pthread_mutex_unlock(&srv->mu);
	http_reject(fd, 503, "out of resources");
	close(fd);
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Stops reading on every open connection, then waits for them to finish. */
static int drain_connections(server *srv)
{
	struct timespec deadline;
	int i, rc = 0;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 10;
	pthread_mutex_lock(&srv->mu);
	for (i = 0; i < srv->max_conns; i++)
		if (srv->conn_fds[i] >= 0)
			shutdown(srv->conn_fds[i], SHUT_RD);
	while (srv->active > 0 && rc == 0)
		rc = pthread_cond_timedwait(&srv->idle, &srv->mu, &deadline);
	rc = srv->active;
	pthread_mutex_unlock(&srv->mu);
	return rc;
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{"plugins", required_argument, NULL, 'd'},
		{"listen", required_argument, NULL, 'l'},
		{"port", required_argument, NULL, 'p'},
		{"scan-interval", required_argument, NULL, 'i'},
		{"max-body", required_argument, NULL, 'b'},
		{"max-connections", required_argument, NULL, 'c'},
		{"verbose", no_argument, NULL, 'v'},
		{"quiet", no_argument, NULL, 'q'},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	const char *dir = "./plugins", *addr = "127.0.0.1";
	long port = 8080, interval = 1000;
	struct sigaction sa;
	char where[NI_MAXHOST + NI_MAXSERV + 16];
	long long next_scan;
	server srv;
	int opt, i, left;

	memset(&srv, 0, sizeof(srv));
	srv.http.max_body = 1 << 20;
	srv.max_conns = 128;
	while ((opt = getopt_long(argc, argv, "d:l:p:i:b:c:vqVh", options, NULL)) != -1) {
		switch (opt) {
		case 'd': dir = optarg; break;
		case 'l': addr = optarg; break;
		case 'p': port = parse_number(optarg, "port", 0, 65535); break;
		case 'i': interval = parse_number(optarg, "scan interval", 0, 86400000); break;
		case 'b': srv.http.max_body = (size_t)parse_number(optarg, "max body", 0, 1L << 30); break;
		case 'c': srv.max_conns = (int)parse_number(optarg, "max connections", 1, 65536); break;
		case 'v': jd_log_level = JD_LOG_DEBUG; break;
		case 'q': jd_log_level = JD_LOG_WARN; break;
		case 'V': printf("jsondaemon " JD_VERSION "\n"); return 0;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (optind < argc) {
		usage(stderr);
		return 2;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal; /* no SA_RESTART: signals interrupt poll() */
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	srv.conn_fds = malloc((size_t)srv.max_conns * sizeof(int));
	if (srv.conn_fds == NULL)
		return 1;
	for (i = 0; i < srv.max_conns; i++)
		srv.conn_fds[i] = -1;
	pthread_mutex_init(&srv.mu, NULL);
	pthread_cond_init(&srv.idle, NULL);

	if (registry_init(&srv.reg, dir) < 0)
		return 1;
	registry_scan(&srv.reg); /* plugins are ready before the first request */

	srv.listen_fd = open_listener(addr, (int)port, where, sizeof(where));
	if (srv.listen_fd < 0) {
		registry_destroy(&srv.reg);
		return 1;
	}
	log_info("jsondaemon %s listening on %s, plugins from %s", JD_VERSION, where, dir);

	next_scan = now_ms() + interval;
	while (!g_stop) {
		struct pollfd pfd = {srv.listen_fd, POLLIN, 0};
		int timeout = 500;

		if (interval > 0) {
			long long wait = next_scan - now_ms();

			timeout = wait < 0 ? 0 : wait < timeout ? (int)wait : timeout;
		}
		if (poll(&pfd, 1, timeout) > 0 && (pfd.revents & POLLIN))
			accept_one(&srv);
		if (g_rescan || (interval > 0 && now_ms() >= next_scan)) {
			if (g_rescan)
				log_info("SIGHUP: rescanning %s", dir);
			g_rescan = 0;
			registry_scan(&srv.reg);
			next_scan = now_ms() + interval;
		}
	}

	log_info("shutting down");
	close(srv.listen_fd);
	left = drain_connections(&srv);
	if (left > 0) {
		/* Plugins may still be in use: leave them loaded, the OS cleans up. */
		log_warn("%d connection(s) still busy, exiting without unloading plugins", left);
		return 0;
	}
	registry_destroy(&srv.reg);
	pthread_cond_destroy(&srv.idle);
	pthread_mutex_destroy(&srv.mu);
	free(srv.conn_fds);
	return 0;
}
