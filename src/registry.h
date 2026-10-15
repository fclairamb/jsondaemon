/*
 * The plugin registry: keeps the set of loaded plugins in sync with the
 * plugin directory. Only the scanning thread modifies it; request threads
 * read it under a read lock and keep that lock for the whole call.
 */
#ifndef JD_REGISTRY_H
#define JD_REGISTRY_H

#include <pthread.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <time.h>

#include "jsondaemon.h"

typedef struct file_sig {
	dev_t dev;
	ino_t ino;
	off_t size;
	time_t mtime;
	long mtime_ns;
} file_sig;

typedef struct plugin {
	char *file;  /* name in the plugin directory */
	char *mount; /* copy of jd_mount() */
	size_t mount_len;
	file_sig sig;
	time_t loaded_at;
	unsigned flags;
	atomic_ulong calls;

	void *handle;
	json_object *(*call)(const char *, json_object *, jd_error *);
	int (*init)(void);
	void (*fini)(void);
	json_object *(*describe)(void);
	pthread_mutex_t call_lock; /* serializes calls unless JD_THREAD_SAFE */

	struct plugin *next;
} plugin;

typedef struct failed_file failed_file;

typedef struct registry {
	pthread_rwlock_t lock;
	pthread_mutex_t gate; /* a waiting writer holds it: no new reader starves it */
	char *dir;
	char *shadow_dir;
	plugin *plugins;
	failed_file *failed;
	unsigned long seq;
	int dir_warned;
} registry;

int registry_init(registry *r, const char *dir);
void registry_destroy(registry *r);

/* Synchronizes the loaded plugins with the directory content. */
void registry_scan(registry *r);

void registry_rdlock(registry *r);
void registry_unlock(registry *r);

/*
 * Under the read lock: finds the plugin with the longest mount prefixing
 * `path` on a segment boundary. `*rest` gets what follows the mount.
 */
plugin *registry_route(registry *r, const char *path, const char **rest);

/* Under the read lock: number of loaded plugins. */
size_t registry_count(registry *r);

/* Under the read lock: plugin_info() of every plugin, sorted by mount. */
json_object *registry_list(registry *r);

/* Calls a plugin, honoring its thread-safety flag. */
json_object *plugin_call(plugin *p, const char *method, json_object *params, jd_error *err);

/* Metadata + jd_describe() output. New reference. */
json_object *plugin_info(plugin *p);

#endif /* JD_REGISTRY_H */
