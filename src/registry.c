#include "registry.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"

/* A file that failed to load: not retried until it changes. */
struct failed_file {
	char *file;
	file_sig sig;
	char *conflict_mount; /* refused because this mount was taken: retried once it is free */
	struct failed_file *next;
};

struct entry {
	char *name;
	file_sig sig;
};

/* dlsym() returns an object pointer; ISO C has no cast to a function pointer. */
#define LOAD_SYMBOL(dst, handle, name)                    \
	do {                                              \
		void *sym_ = dlsym((handle), (name));     \
		memcpy(&(dst), &sym_, sizeof(dst));       \
	} while (0)

static char *path_join(const char *dir, const char *name)
{
	size_t len = strlen(dir) + strlen(name) + 2;
	char *path = malloc(len);

	if (path != NULL)
		snprintf(path, len, "%s/%s", dir, name);
	return path;
}

static void sig_from_stat(file_sig *sig, const struct stat *st)
{
	sig->dev = st->st_dev;
	sig->ino = st->st_ino;
	sig->size = st->st_size;
	sig->mtime = st->st_mtime;
#if defined(__APPLE__)
	sig->mtime_ns = st->st_mtimespec.tv_nsec;
#else
	sig->mtime_ns = st->st_mtim.tv_nsec;
#endif
}

static int sig_equal(const file_sig *a, const file_sig *b)
{
	return a->dev == b->dev && a->ino == b->ino && a->size == b->size && a->mtime == b->mtime &&
	       a->mtime_ns == b->mtime_ns;
}

static const char *mount_problem(const char *m)
{
	size_t len;
	const char *p;

	if (m == NULL)
		return "jd_mount() returned NULL";
	len = strlen(m);
	if (m[0] != '/')
		return "it must start with '/'";
	if (len < 2)
		return "the root '/' is reserved for the daemon";
	if (len > 128)
		return "it is longer than 128 characters";
	if (m[len - 1] == '/')
		return "it must not end with '/'";
	if (m[1] == '_')
		return "paths starting with '/_' are reserved for the daemon";
	for (p = m; *p != '\0'; p++) {
		if (*p == '/') {
			if (p[1] == '/')
				return "it must not contain '//'";
			if (p[1] == '.' && (p[2] == '/' || p[2] == '\0' || (p[2] == '.' && (p[3] == '/' || p[3] == '\0'))))
				return "it must not contain '.' or '..' segments";
			continue;
		}
		if (!isalnum((unsigned char)*p) && strchr("-._~", *p) == NULL)
			return "it may only contain letters, digits, '/' and - . _ ~";
	}
	return NULL;
}

int registry_init(registry *r, const char *dir)
{
	const char *tmp = getenv("TMPDIR");
	char *tmpl;

	memset(r, 0, sizeof(*r));
	if (tmp == NULL || *tmp == '\0')
		tmp = "/tmp";
	tmpl = path_join(tmp, "jsondaemon.XXXXXX");
	if (tmpl == NULL || mkdtemp(tmpl) == NULL) {
		log_error("cannot create a shadow directory in %s: %s", tmp, strerror(errno));
		free(tmpl);
		return -1;
	}
	r->shadow_dir = tmpl;
	r->dir = strdup(dir);
	pthread_rwlock_init(&r->lock, NULL);
	pthread_mutex_init(&r->gate, NULL);
	return 0;
}

static void plugin_close(plugin *p, int started)
{
	if (started && p->fini != NULL)
		p->fini();
	if (p->handle != NULL)
		dlclose(p->handle);
	pthread_mutex_destroy(&p->call_lock);
	free(p->file);
	free(p->mount);
	free(p);
}

/* Copies src into a fresh file of the shadow directory. */
static int copy_file(int in, const char *dst, off_t expected, int *transient)
{
	char chunk[65536];
	off_t copied = 0;
	int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0700);

	if (out < 0) {
		log_error("cannot create %s: %s", dst, strerror(errno));
		*transient = 1;
		return -1;
	}
	for (;;) {
		ssize_t n = read(in, chunk, sizeof(chunk)), w = 0;

		if (n == 0)
			break;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			goto fail;
		}
		while (w < n) {
			ssize_t k = write(out, chunk + w, (size_t)(n - w));

			if (k < 0) {
				if (errno == EINTR)
					continue;
				goto fail;
			}
			w += k;
		}
		copied += n;
	}
	if (close(out) < 0) {
		out = -1;
		goto fail;
	}
	if (copied != expected) {
		/* Still being written: try again on the next scan. */
		*transient = 1;
		unlink(dst);
		return -1;
	}
	return 0;
fail:
	*transient = 1;
	if (out >= 0)
		close(out);
	unlink(dst);
	return -1;
}

/*
 * Loads a plugin and asks where it wants to be mounted. The file is copied
 * aside first and the copy is loaded: the original can then be overwritten
 * in place (cp, a compiler...) without crashing the code being run.
 * Returns NULL on failure; *transient is set when it is worth retrying.
 */
static plugin *plugin_open(registry *r, const char *file, int *transient)
{
	char *src = path_join(r->dir, file), *shadow = NULL, name[64];
	const char *(*mount_fn)(void) = NULL;
	const char *mount = NULL, *problem;
	const unsigned *flags;
	plugin *p = NULL;
	void *handle = NULL;
	struct stat st;
	int in = -1;

	*transient = 0;
	if (src == NULL)
		goto fail;
	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0 || fstat(in, &st) < 0) {
		*transient = 1;
		goto fail;
	}
	snprintf(name, sizeof(name), "%lu.so", ++r->seq);
	shadow = path_join(r->shadow_dir, name);
	if (shadow == NULL || copy_file(in, shadow, st.st_size, transient) < 0)
		goto fail;

	handle = dlopen(shadow, RTLD_NOW | RTLD_LOCAL);
	unlink(shadow); /* the mapping outlives the file */
	if (handle == NULL) {
		log_error("%s: cannot load: %s", file, dlerror());
		goto fail;
	}

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		goto fail;
	LOAD_SYMBOL(mount_fn, handle, "jd_mount");
	LOAD_SYMBOL(p->call, handle, "jd_call");
	if (mount_fn == NULL || p->call == NULL) {
		log_error("%s: not a jsondaemon plugin, it does not export %s", file,
		          mount_fn == NULL ? "jd_mount()" : "jd_call()");
		goto fail;
	}
	LOAD_SYMBOL(p->init, handle, "jd_init");
	LOAD_SYMBOL(p->fini, handle, "jd_fini");
	LOAD_SYMBOL(p->describe, handle, "jd_describe");
	flags = dlsym(handle, "jd_flags");
	p->flags = flags != NULL ? *flags : 0;

	mount = mount_fn();
	problem = mount_problem(mount);
	if (problem != NULL) {
		log_error("%s: refused mount \"%s\": %s", file, mount != NULL ? mount : "(null)", problem);
		goto fail;
	}
	p->mount = strdup(mount);
	p->file = strdup(file);
	if (p->mount == NULL || p->file == NULL)
		goto fail;
	p->mount_len = strlen(p->mount);
	p->handle = handle;
	sig_from_stat(&p->sig, &st);
	atomic_init(&p->calls, 0);
	pthread_mutex_init(&p->call_lock, NULL);

	close(in);
	free(src);
	free(shadow);
	return p;

fail:
	if (p != NULL) {
		free(p->mount);
		free(p->file);
		free(p);
	}
	if (handle != NULL)
		dlclose(handle);
	if (in >= 0)
		close(in);
	free(src);
	free(shadow);
	return NULL;
}

static int plugin_start(plugin *p)
{
	int rc;

	if (p->init == NULL)
		return 0;
	rc = p->init();
	if (rc != 0) {
		log_error("%s: jd_init() failed (returned %d)", p->file, rc);
		return -1;
	}
	return 0;
}

static int has_plugin_suffix(const char *name)
{
	size_t len = strlen(name);

	return (len > 3 && strcmp(name + len - 3, ".so") == 0) || (len > 6 && strcmp(name + len - 6, ".dylib") == 0);
}

static int entry_cmp(const void *a, const void *b)
{
	return strcmp(((const struct entry *)a)->name, ((const struct entry *)b)->name);
}

/* Lists the candidate plugin files, sorted by name. -1 if the scan must be skipped. */
static int list_dir(registry *r, struct entry **out, size_t *count)
{
	struct entry *entries = NULL;
	size_t n = 0, cap = 0;
	struct dirent *de;
	DIR *d = opendir(r->dir);

	*out = NULL;
	*count = 0;
	if (d == NULL) {
		if (errno != ENOENT)
			return -1; /* EMFILE, EACCES...: do not unload everything */
		if (!r->dir_warned)
			log_warn("plugin directory %s does not exist (yet)", r->dir);
		r->dir_warned = 1;
		return 0;
	}
	r->dir_warned = 0;
	while ((de = readdir(d)) != NULL) {
		struct stat st;
		char *path;

		if (de->d_name[0] == '.' || !has_plugin_suffix(de->d_name))
			continue;
		path = path_join(r->dir, de->d_name);
		if (path == NULL || stat(path, &st) < 0 || !S_ISREG(st.st_mode)) {
			free(path);
			continue;
		}
		free(path);
		if (n == cap) {
			struct entry *grown = realloc(entries, (cap = cap ? cap * 2 : 16) * sizeof(*entries));

			if (grown == NULL)
				break;
			entries = grown;
		}
		entries[n].name = strdup(de->d_name);
		if (entries[n].name == NULL)
			break;
		sig_from_stat(&entries[n].sig, &st);
		n++;
	}
	closedir(d);
	if (n > 1)
		qsort(entries, n, sizeof(*entries), entry_cmp);
	*out = entries;
	*count = n;
	return 0;
}

static int entry_key_cmp(const void *key, const void *e)
{
	return strcmp(key, ((const struct entry *)e)->name);
}

static struct entry *find_entry(struct entry *entries, size_t n, const char *name)
{
	return n == 0 ? NULL : bsearch(name, entries, n, sizeof(*entries), entry_key_cmp);
}

static plugin *find_file(plugin *list, const char *file)
{
	for (; list != NULL; list = list->next)
		if (strcmp(list->file, file) == 0)
			return list;
	return NULL;
}

static int contains(plugin **set, size_t n, const plugin *p)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (set[i] == p)
			return 1;
	return 0;
}

/* Who owns `mount` once the pending changes are applied, ignoring `self`. */
static const char *mount_owner(registry *r, const char *mount, plugin **drop, size_t ndrop, plugin **add, size_t nadd,
                               const plugin *self)
{
	plugin *p;
	size_t i;

	for (p = r->plugins; p != NULL; p = p->next)
		if (p != self && !contains(drop, ndrop, p) && strcmp(p->mount, mount) == 0)
			return p->file;
	for (i = 0; i < nadd; i++)
		if (strcmp(add[i]->mount, mount) == 0)
			return add[i]->file;
	return NULL;
}

static failed_file *find_failed(registry *r, const char *file)
{
	failed_file *f;

	for (f = r->failed; f != NULL; f = f->next)
		if (strcmp(f->file, file) == 0)
			return f;
	return NULL;
}

static void forget_failed(registry *r, const char *file)
{
	failed_file **pp, *f;

	for (pp = &r->failed; (f = *pp) != NULL; pp = &f->next) {
		if (strcmp(f->file, file) == 0) {
			*pp = f->next;
			free(f->file);
			free(f->conflict_mount);
			free(f);
			return;
		}
	}
}

static void remember_failed(registry *r, const struct entry *e, const char *conflict_mount)
{
	failed_file *f;

	forget_failed(r, e->name);
	f = calloc(1, sizeof(*f));
	if (f == NULL)
		return;
	f->file = strdup(e->name);
	f->sig = e->sig;
	f->conflict_mount = conflict_mount != NULL ? strdup(conflict_mount) : NULL;
	f->next = r->failed;
	r->failed = f;
}

static void write_lock(registry *r)
{
	pthread_mutex_lock(&r->gate);
	pthread_rwlock_wrlock(&r->lock);
	pthread_mutex_unlock(&r->gate);
}

void registry_scan(registry *r)
{
	struct entry *entries;
	size_t n, i, ndrop = 0, nadd = 0, nloaded = 0;
	plugin **drop, **add, *p;
	failed_file **fp, *f;
	int pass;

	if (list_dir(r, &entries, &n) < 0) {
		log_warn("cannot read plugin directory %s: %s", r->dir, strerror(errno));
		return;
	}
	for (p = r->plugins; p != NULL; p = p->next)
		nloaded++;
	drop = calloc(nloaded + 1, sizeof(*drop));
	add = calloc(n + 1, sizeof(*add));
	if (drop == NULL || add == NULL)
		goto out;

	/* Files that are gone. */
	for (p = r->plugins; p != NULL; p = p->next)
		if (find_entry(entries, n, p->file) == NULL)
			drop[ndrop++] = p;

	/* Pass 0 reloads files that changed, pass 1 loads new ones. */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < n; i++) {
			struct entry *e = &entries[i];
			plugin *old = find_file(r->plugins, e->name), *np;
			const char *owner;
			int transient;

			if ((pass == 0) != (old != NULL))
				continue;
			if (old != NULL && sig_equal(&old->sig, &e->sig))
				continue;
			f = find_failed(r, e->name);
			if (f != NULL && sig_equal(&f->sig, &e->sig) &&
			    (f->conflict_mount == NULL ||
			     mount_owner(r, f->conflict_mount, drop, ndrop, add, nadd, old) != NULL))
				continue;

			np = plugin_open(r, e->name, &transient);
			if (np == NULL) {
				if (!transient)
					remember_failed(r, e, NULL);
				if (old != NULL && !transient)
					log_warn("%s: keeping the version loaded before", e->name);
				continue;
			}
			owner = mount_owner(r, np->mount, drop, ndrop, add, nadd, old);
			if (owner != NULL) {
				log_error("%s: mount %s is already taken by %s", e->name, np->mount, owner);
				remember_failed(r, e, np->mount);
				plugin_close(np, 0);
				continue;
			}
			if (plugin_start(np) < 0) {
				remember_failed(r, e, NULL);
				plugin_close(np, 0);
				if (old != NULL)
					log_warn("%s: keeping the version loaded before", e->name);
				continue;
			}
			np->loaded_at = time(NULL);
			forget_failed(r, e->name);
			if (old != NULL)
				drop[ndrop++] = old;
			add[nadd++] = np;
		}
	}

	/* Forget about failed files that were removed. */
	for (fp = &r->failed; (f = *fp) != NULL;) {
		if (find_entry(entries, n, f->file) == NULL) {
			*fp = f->next;
			free(f->file);
			free(f->conflict_mount);
			free(f);
		} else {
			fp = &f->next;
		}
	}

	if (ndrop == 0 && nadd == 0)
		goto out;

	/* Publish. Waits for the calls in flight, which hold the read lock. */
	write_lock(r);
	{
		plugin **pp = &r->plugins;

		while (*pp != NULL) {
			if (contains(drop, ndrop, *pp))
				*pp = (*pp)->next;
			else
				pp = &(*pp)->next;
		}
		for (i = 0; i < nadd; i++) {
			add[i]->next = r->plugins;
			r->plugins = add[i];
		}
	}
	pthread_rwlock_unlock(&r->lock);

	for (i = 0; i < nadd; i++) {
		size_t j;
		int reloaded = 0;

		for (j = 0; j < ndrop; j++)
			reloaded |= strcmp(drop[j]->file, add[i]->file) == 0;
		log_info("%s %s at %s%s", reloaded ? "reloaded" : "loaded", add[i]->file, add[i]->mount,
		         add[i]->flags & JD_THREAD_SAFE ? " (thread-safe)" : "");
	}
	for (i = 0; i < ndrop; i++) {
		if (find_file(r->plugins, drop[i]->file) == NULL)
			log_info("unloaded %s from %s", drop[i]->file, drop[i]->mount);
		plugin_close(drop[i], 1);
	}

out:
	for (i = 0; i < n; i++)
		free(entries[i].name);
	free(entries);
	free(drop);
	free(add);
}

void registry_destroy(registry *r)
{
	plugin *p, *next;
	failed_file *f, *fnext;

	write_lock(r);
	p = r->plugins;
	r->plugins = NULL;
	pthread_rwlock_unlock(&r->lock);
	for (; p != NULL; p = next) {
		next = p->next;
		log_debug("unloaded %s from %s", p->file, p->mount);
		plugin_close(p, 1);
	}
	for (f = r->failed; f != NULL; f = fnext) {
		fnext = f->next;
		free(f->file);
		free(f->conflict_mount);
		free(f);
	}
	if (r->shadow_dir != NULL)
		rmdir(r->shadow_dir);
	free(r->shadow_dir);
	free(r->dir);
	pthread_rwlock_destroy(&r->lock);
	pthread_mutex_destroy(&r->gate);
}

void registry_rdlock(registry *r)
{
	pthread_mutex_lock(&r->gate);
	pthread_rwlock_rdlock(&r->lock);
	pthread_mutex_unlock(&r->gate);
}

void registry_unlock(registry *r)
{
	pthread_rwlock_unlock(&r->lock);
}

plugin *registry_route(registry *r, const char *path, const char **rest)
{
	plugin *p, *best = NULL;

	for (p = r->plugins; p != NULL; p = p->next) {
		if (strncmp(path, p->mount, p->mount_len) != 0)
			continue;
		if (path[p->mount_len] != '\0' && path[p->mount_len] != '/')
			continue;
		if (best == NULL || p->mount_len > best->mount_len)
			best = p;
	}
	if (best != NULL)
		*rest = path + best->mount_len;
	return best;
}

size_t registry_count(registry *r)
{
	size_t n = 0;
	plugin *p;

	for (p = r->plugins; p != NULL; p = p->next)
		n++;
	return n;
}

json_object *plugin_call(plugin *p, const char *method, json_object *params, jd_error *err)
{
	int serialize = !(p->flags & JD_THREAD_SAFE);
	json_object *result;

	memset(err, 0, sizeof(*err));
	if (serialize)
		pthread_mutex_lock(&p->call_lock);
	result = p->call(method, params, err);
	if (serialize)
		pthread_mutex_unlock(&p->call_lock);
	atomic_fetch_add(&p->calls, 1);

	err->message[sizeof(err->message) - 1] = '\0';
	if (err->code != 0 && result != NULL) {
		json_object_put(result);
		result = NULL;
	}
	if (err->code == 0 && err->data != NULL) {
		json_object_put(err->data);
		err->data = NULL;
	}
	return result;
}

json_object *plugin_info(plugin *p)
{
	json_object *info = json_object_new_object();
	char stamp[32];
	struct tm tm;

	gmtime_r(&p->loaded_at, &tm);
	strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm);
	json_object_object_add(info, "mount", json_object_new_string(p->mount));
	json_object_object_add(info, "file", json_object_new_string(p->file));
	json_object_object_add(info, "loaded_at", json_object_new_string(stamp));
	json_object_object_add(info, "calls", json_object_new_int64((int64_t)atomic_load(&p->calls)));
	json_object_object_add(info, "thread_safe", json_object_new_boolean(p->flags & JD_THREAD_SAFE));
	if (p->describe != NULL) {
		int serialize = !(p->flags & JD_THREAD_SAFE);
		json_object *described;

		if (serialize)
			pthread_mutex_lock(&p->call_lock);
		described = p->describe();
		if (serialize)
			pthread_mutex_unlock(&p->call_lock);
		if (described != NULL)
			json_object_object_add(info, "about", described);
	}
	return info;
}

static int plugin_mount_cmp(const void *a, const void *b)
{
	return strcmp((*(plugin *const *)a)->mount, (*(plugin *const *)b)->mount);
}

json_object *registry_list(registry *r)
{
	size_t n = registry_count(r), i = 0;
	plugin **sorted = calloc(n + 1, sizeof(*sorted)), *p;
	json_object *list = json_object_new_array();

	if (sorted == NULL)
		return list;
	for (p = r->plugins; p != NULL; p = p->next)
		sorted[i++] = p;
	qsort(sorted, n, sizeof(*sorted), plugin_mount_cmp);
	for (i = 0; i < n; i++)
		json_object_array_add(list, plugin_info(sorted[i]));
	free(sorted);
	return list;
}
