#include <lib/platform.h>
#include <lib/sleep.h>
#include <lib/jiffies.h>

#include <pthread.h>
#include <unistd.h>
#include <fuse/fuse_lowlevel.h>

#include <kfs/modman.h>
#include <kfs/fuse_serve_mount.h>

// High level overview:
// The complex aspect of fuse_serve_mount is that fuse_mount() and
// fuse_unmount() block to obtain data from mountpoint's parent
// filesystem. As fuse_serve allows nested mounts one must either
// rewrite these two fuse functions to be event driven or run them
// within a second thread. fuse_serve_mount uses a second thread.
//
// Most of fuse_serve_mount does not use knowledge of the
// mount/unmount thread or the related synchronization operations;
// functions defined at the top of this file provide interfaces to the
// multithreading world.
//
// Mount and unmount operations, up to fuse_mount() and
// fuse_unmount(), are done in in sequentially. The operation is then
// added to a queue that helper_thread draws from to complete the
// operation.


#define FUSE_SERVE_MOUNT_DEBUG 0

#if FUSE_SERVE_MOUNT_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

// Max number of seconds to wait for the helper thread when shutting down
#define MAX_HELPER_SHUTDOWN_WAIT 4
// Max number of seconds to wait for the helper thread to finish when
// starting a shutdown
#define MAX_START_SHUTDOWN_WAIT 4

static mount_t ** mounts = NULL; // set of mount_t*
static int nmounts = 0;
static mount_t * root = NULL;

struct fuse_lowlevel_ops * ops;
size_t ops_len;

static bool root_service_started;

static int unmount_pipe[2];

typedef struct queue_entry {
	mount_t * mount;
	enum { QEMOUNT, QEUNMOUNT } action;
} queue_entry_t;

static vector_t * remove_queue; // vector of queue_entry_t*

static int helper_init(void);
static int helper_shutdown(void);

static int enqueue_helper_request(queue_entry_t * qe);
static int ensure_helper_is_running(void);
static bool shutdown_has_started(void);


mount_t ** fuse_serve_mounts(void)
{
	return mounts;
}

static void mounts_insert(mount_t *m)
{
	mount_t **mp;
	for (mp = mounts; mp && *mp && *mp != m; mp++)
		/* nada */;
	if (!mp || !*mp) {
		mounts = (mount_t **) realloc(mounts, (nmounts + 2) * sizeof(*mounts));
		mounts[nmounts] = m;
		mounts[++nmounts] = NULL;
	}
}

static void mounts_remove(mount_t *m)
{
	mount_t **mp;
	for (mp = mounts; mp && *mp; mp++)
		if (*mp == m) {
			*mp = mounts[--nmounts];
			mounts[nmounts] = NULL;
			break;
		}
}

size_t fuse_serve_mount_chan_bufsize(void)
{
	assert(root && root->mounted);
	return fuse_chan_bufsize(root->channel);
}

int fuse_serve_mount_set_root(CFS_t * root_cfs)
{
	int r;
	Dprintf("%s(%s)\n", __FUNCTION__, modman_name_cfs(root_cfs));
	if (!root)
		return -1;
	if (root_service_started)
		return -EBUSY;

	if ((r = CALL(root_cfs, get_root, &root->root_ino)) < 0)
		return r;

	if ((r = hash_map_insert(root->parents, (void *) root->root_ino, (void *) root->root_ino)) < 0)
		return r;

	root->cfs = root_cfs;
	printf("Mounted \"/\" from %s\n", modman_name_cfs(root_cfs));
	return 0;
}

int fuse_serve_mount_load_mounts(void)
{
	if (!root || !root->mounted)
		return -1;
	root_service_started = 1;
	return ensure_helper_is_running();
}



// A qsort compar function to order paths by directories and then filenames
static int path_compar(const void * p1, const void * p2)
{
	const char * p1s = p1;
	const char * p2s = p2;

	if (!p1 && !p2)
		return 0;
	else if (!p1)
		return -1;
	else if (!p2)
		return 1;

	while (1)
	{
		const char * p1e;
		const char * p2e;
		size_t p1_len, p2_len;
		int r;

		p1e = strchr(p1s, '/');
		p2e = strchr(p2s, '/');

		if (!p1e && !p2e)
			return strcmp(p1s, p2s);
		else if (!p1e)
			return -1;
		else if (!p2e)
			return 1;
	
		p1_len = p1e - p1s;
		p2_len = p2e - p2s;
		if (p1_len < p2_len)
			return -1;
		else if (p2_len < p1_len)
			return 1;

		if ((r = strncmp(p1s, p2s, p1_len)))
			return r;

		p1s = p1e + 1;
		p2s = p2e + 1;
	}
}

// A qsort compar function to order mounts by paths
static int mount_path_compar(const void * v1, const void * v2)
{
	const mount_t * m1 = *((const mount_t **) v1);
	const mount_t * m2 = *((const mount_t **) v2);
	if (!m1 && !m2)
		return 0;
	else if (!m1)
		return -1;
	else if (!m2)
		return 1;

	return path_compar(m1->kfs_path, m2->kfs_path);
}

static int fuse_args_copy(const struct fuse_args * src, struct fuse_args * copy)
{
	int argv_len = 0;
	int i;

	// Assert we are initing the whole structure
	static_assert(sizeof(*src) == sizeof(src->argc) + sizeof(src->argv) + sizeof(src->allocated));

	copy->argc = src->argc;

	while (src->argv[argv_len++]);

	if (!(copy->argv = malloc(argv_len * sizeof(char*))))
		return -ENOMEM;
	for (i = 0; i < argv_len - 1; i++)
		if (!(copy->argv[i] = strdup(src->argv[i])))
			goto error;
	copy->argv[i] = 0;

	copy->allocated = 1;

	return 0;

  error:
	while (--i >= 0)
		free(copy->argv[i]);
	free(copy->argv);
	return -ENOMEM;
}

int fuse_serve_mount_add(CFS_t * cfs, const char * path)
{
	mount_t * m;
	queue_entry_t * qe;
	int r;
	Dprintf("%s(%s, \"%s\")\n", __FUNCTION__, modman_name_cfs(cfs), path);

	if (shutdown_has_started())
		return -EBUSY; // We might be able to allow this; but at least for now, reject

	if (!(m = calloc(1, sizeof(*m))))
		return -ENOMEM;
	if (!(qe = calloc(1, sizeof(*qe))))
	{
		r = -ENOMEM;
		goto error_m;
	}

	qe->mount = m;
	qe->action = QEMOUNT;

	m->mounted = 0;

	if (!(m->kfs_path = strdup(path)))
	{
		r = -ENOMEM;
		goto error_qe;
	}

	if (!(m->parents = hash_map_create()))
	{
		r = -ENOMEM;
		goto error_path;
	}

	m->cfs = cfs;

	if ((r = CALL(cfs, get_root, &m->root_ino)) < 0)
		goto error_parents;

	if ((r = hash_map_insert(m->parents, (void *) m->root_ino, (void *) m->root_ino)) < 0)
		goto error_parents;

	if ((r = fuse_args_copy(&root->args, &m->args)) < 0)
		goto error_parents;

	m->mountpoint = malloc(strlen(root->mountpoint) + strlen(path) + 1);
	if (!m->mountpoint)
	{
		r = -ENOMEM;
		goto error_args;
	}
	strcpy(m->mountpoint, root->mountpoint);
	strcpy(m->mountpoint + strlen(root->mountpoint), path);

	// add to mounts list
	mounts_insert(m);

	// helper_thread takes care of the channel_fd field and on down
	if (enqueue_helper_request(qe))
		goto error_insert;
	if (ensure_helper_is_running() < 0)
	{
		// As it is not expected that ensure_helper_is_running() will error
		// and as recovering would require a single-use dequeue function,
		// for now we just error and let things go as they will.
		fprintf(stderr, "%s: ensure_helper_is_running failed. WARNING: request remains in the queue.\n", __FUNCTION__);
		goto error_insert;
	}

	return 0;

  error_insert:
	mounts_remove(m);
	free(m->mountpoint);
  error_args:
	fuse_opt_free_args(&m->args);
  error_parents:
	hash_map_destroy(m->parents);
  error_path:
	free(m->kfs_path);
  error_qe:
	memset(qe, 0, sizeof(*qe));
	free(qe);
  error_m:
	memset(m, 0, sizeof(*m));
	free(m);
	return r;
}

int fuse_serve_mount_remove(mount_t * m)
{
	queue_entry_t * qe;
	char b = 1;
	Dprintf("%s(\"%s\")\n", __FUNCTION__, m->kfs_path);

	if (!m || !m->mounted)
		return -EINVAL;

	if (shutdown_has_started())
		return 0; // m is already scheduled to be unmounted

	if (!(qe = calloc(1, sizeof(*qe))))
		return -ENOMEM;
	qe->mount = m;
	qe->action = QEUNMOUNT;

	if (write(unmount_pipe[1], &b, 1) != 1)
	{
		perror("fuse_serve_mount_remove(): write");
		return -1;
	}

	return vector_push_back(remove_queue, qe);
}


static int mount_root(int argc, char ** argv)
{
	Dprintf("%s()\n", __FUNCTION__);

	if (!(root = calloc(1, sizeof(*root))))
		return -ENOMEM;

	// We can't use FUSE_ARGS_INIT() here so assert we are initing the
	// whole structure
	static_assert(sizeof(root->args) == sizeof(argc) + sizeof(argv) + sizeof(int));
	root->args.argc = argc;
	root->args.argv = argv;
	root->args.allocated = 0;

	if (!(root->kfs_path = strdup("")))
		return -ENOMEM;

	if (!(root->parents = hash_map_create()))
		return -ENOMEM;

	root->cfs = NULL; // set later via fuse_serve_mount_set_root()

	if (fuse_parse_cmdline(&root->args, &root->mountpoint, NULL, NULL) == -1)
	{
		fprintf(stderr, "%s(): fuse_parse_cmdline() failed\n", __FUNCTION__);
		return -1;
	}

	if ((root->channel_fd = fuse_mount(root->mountpoint, &root->args)) == -1)
	{
		fprintf(stderr, "%s():%d: fuse_mount(\"%s\") failed\n", __FUNCTION__, __LINE__, root->mountpoint);
		return -1;
	}

	if (!(root->session = fuse_lowlevel_new(&root->args, ops, ops_len, root)))
	{
		fprintf(stderr, "%s(): fuse_lowlevel_new() failed\n", __FUNCTION__);
		return -1;
	}

	if (!(root->channel = fuse_kern_chan_new(root->channel_fd)))
	{
		fprintf(stderr, "%s(): fuse_kern_chan_new() failed\n", __FUNCTION__);
		return -1;
	}

	fuse_session_add_chan(root->session, root->channel);

	mounts_insert(root);

	root->mounted = 1;

	return 0;
}

// Destroy variables local to fuse_serve_mount
static void destroy_locals()
{
	free(mounts); // destroy even if non-empty
	mounts = NULL;
	vector_destroy(remove_queue);
	remove_queue = NULL;
	root = NULL; // should already be unmounted
	ops = NULL;
	ops_len = 0;
	root_service_started = 0;
	if (unmount_pipe[1] >= 0)
		close(unmount_pipe[1]);
	unmount_pipe[1] = -1;
	unmount_pipe[0] = -1;
}

static int unmount_root(void)
{
	int r;

	if (!root->mounted)
		return -EINVAL;

	mounts_remove(root);

	if (root->session)
		fuse_session_destroy(root->session); // also destroys root->channel
	if (root->channel_fd >= 0)
		(void) close(root->channel_fd);

	// only use fuse_unmount if there are no nested mounts
	if (nmounts == 0)
		fuse_unmount(root->mountpoint);

	fuse_opt_free_args(&root->args);

	free(root->mountpoint);
	free(root->kfs_path);
	hash_map_destroy(root->parents);

	memset(root, 0, sizeof(*root));
	free(root);
	root = NULL;

	if ((r = helper_shutdown()) < 0)
		fprintf(stderr, "%s(): helper_shutdown() failed (%d), continuing anyway\n", __FUNCTION__, r);

	destroy_locals();

	return 0;
}


int fuse_serve_mount_init(int argc, char ** argv, struct fuse_lowlevel_ops * _ops, size_t _ops_len)
{
	int r;
	Dprintf("%s()\n", __FUNCTION__);
	assert(!root);

	root_service_started = 0;

	if (pipe(unmount_pipe) == -1)
	{
		perror("fuse_serve_mount_init(): pipe");
		r = -1;
		goto error_mounts;
	}

	if (!(remove_queue = vector_create()))
	{
		r = -ENOMEM;
		goto error_pipe;
	}

	if ((r = helper_init()) < 0)
		goto error_remove_queue;

	ops = _ops;
	ops_len = _ops_len;

	if ((r = mount_root(argc, argv)) < 0)
		goto error_helper;

	return unmount_pipe[0];

  error_helper:
	(void) helper_shutdown();
  error_remove_queue:
	vector_destroy(remove_queue);
	remove_queue = NULL;
  error_pipe:
	(void) close(unmount_pipe[0]);
	(void) close(unmount_pipe[1]);
	unmount_pipe[0] = -1;
	unmount_pipe[1] = -1;
  error_mounts:
	free(mounts);
	mounts = NULL;
	return r;
}

void fuse_serve_mount_instant_shutdown(void)
{
	int r;
	Dprintf("%s()\n", __FUNCTION__);

	if (!mounts)
		return; // already shutdown

	if (nmounts == 1)
	{
		r = unmount_root();
		assert(r >= 0);
	}
	else
	{
		// As we are doing an instant shutdown we cannot do a piecemeal unmount
		// Instead, exec fusermount to do a lazy unmount of the entire tree

		const char * unmount_templ = "fusermount -u -z -- ";
		char * unmount = malloc(strlen(unmount_templ) + strlen(root->mountpoint) + 1);
		if (!unmount)
			perror("fuse_serve_mount_instant_shutdown(): malloc");
		else
		{
			unmount[0] = 0;
			strcat(unmount, unmount_templ);
			strcat(unmount, root->mountpoint);
			if ((r = system(unmount)) < 0)
				fprintf(stderr, "system(\"%s\") = %d\n", unmount, r);
			free(unmount);
		}

		r = unmount_root();
		assert(r >= 0);
	}
}

int fuse_serve_mount_step_remove(void)
{
	char b = 1;
	queue_entry_t * qe;
	Dprintf("%s()\n", __FUNCTION__);

	if (unmount_pipe[0] == -1)
		return -1;

	// Read the byte from helper to zero the read fd's level
	if (read(unmount_pipe[0], &b, 1) != 1)
	{
		perror("fuse_serve_mount_step_shutdown(): read");
		if (write(unmount_pipe[1], &b, 1) != 1)
			assert(0);
		return -1;
	}

	if (vector_size(remove_queue) > 0)
	{
		qe = vector_elt(remove_queue, 0);
		// NOTE: vector_erase() is O(|remove_queue|). If this queue
		// gets to be big we can change how this removal works.
		vector_erase(remove_queue, 0);
	}
	else
	{
		assert(shutdown_has_started());

		if (nmounts == 1)
		{
			Dprintf("%s(): unmounting root\n", __FUNCTION__);
			return unmount_root();
		}

		if (!(qe = calloc(1, sizeof(*qe))))
		{
			(void) write(unmount_pipe[1], &b, 1); // unzero the read fd's level
			return -ENOMEM;
		}
		qsort(mounts, nmounts, sizeof(*mounts), mount_path_compar);
		qe->mount = mounts[nmounts - 1];
		qe->action = QEUNMOUNT;
	}

	mounts_remove(qe->mount);

	fuse_session_destroy(qe->mount->session);
	qe->mount->session = NULL;

	(void) close(qe->mount->channel_fd);

	fuse_opt_free_args(&qe->mount->args);

	if (enqueue_helper_request(qe) < 0)
	{
		fprintf(stderr, "%s(): enqueue_helper_request failed; unmount \"%s\" is unrecoverable\n", __FUNCTION__, qe->mount->kfs_path);
		free(qe);
		return -1;
	}
	if (ensure_helper_is_running() < 0)
	{
		fprintf(stderr, "%s(): ensure_helper_is_running failed; unmount \"%s\" is unrecoverable\n", __FUNCTION__, qe->mount->kfs_path);
		return -1;
	}

	return 0;
}



//
// Begin multithread-aware code and data
//
// There exists up to one thread that runs helper_thread().
// Requests are queued to helper_thread using helper.queue and all helper
// interactions use the mutex helper.mutex.


// Do a mount for helper_thread()
static void helper_thread_mount(mount_t * m)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, m->kfs_path);

	if ((m->channel_fd = fuse_mount(m->mountpoint, &m->args)) == -1)
	{
		fprintf(stderr, "%s(): fuse_mount(\"%s\") failed. (Does the mountpoint exist?)\n", __FUNCTION__, m->mountpoint);
		return;
	}

	if (!(m->session = fuse_lowlevel_new(&m->args, ops, ops_len, m)))
	{
		fprintf(stderr, "%s(): fuse_lowlevel_new() failed\n", __FUNCTION__);
		return;
	}

	if (!(m->channel = fuse_kern_chan_new(m->channel_fd)))
	{
		fprintf(stderr, "%s(): fuse_kern_chan_new() failed\n", __FUNCTION__);
		return;
	}

	fuse_session_add_chan(m->session, m->channel);

	if (fuse_chan_bufsize(m->channel) != fuse_serve_mount_chan_bufsize())
		fprintf(stderr, "bufsizes differ!\n");

	m->mounted = 1;

	printf("Mounted \"%s\" from %s\n", m->kfs_path, modman_name_cfs(m->cfs));
}

// Do an unmount for helper_thread()
static void helper_thread_unmount(mount_t * m)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, m->kfs_path);
	fuse_unmount(m->mountpoint);
	free(m->mountpoint);
	free(m->kfs_path);
	hash_map_destroy(m->parents);
	memset(m, 0, sizeof(*m));
	free(m);
}


// The struct used for base and helper thread interactions
static struct {
	pthread_mutex_t mutex;
	bool alive;
	vector_t * queue; // vector of queue_entry_t*
	bool shutdown_started;
} helper;

static int helper_init(void)
{
	int r;

	if (helper.alive || helper.shutdown_started || helper.queue)
		return -EBUSY;

	if ((r = pthread_mutex_init(&helper.mutex, NULL)) < 0)
	{
		fprintf(stderr, "%s(): pthread_mutex_init: %d\n", __FUNCTION__, r);
		return r;
	}

	if (!(helper.queue = vector_create()))
	{
		(void) pthread_mutex_destroy(&helper.mutex);
		return -ENOMEM;
	}
	
	helper.alive = 0;
	helper.shutdown_started = 0;
	return r;
}

static int helper_shutdown(void)
{
	int r = 0;

	while (helper.alive)
	{
		if (++r > 4*MAX_HELPER_SHUTDOWN_WAIT)
		{
			fprintf(stderr, "%s(): helper thread does not seem to be exiting, continuing shutdown behind its back.\n", __FUNCTION__);
			break;
		}
		jsleep(HZ / 4);
	}

	if ((r = pthread_mutex_destroy(&helper.mutex)) < 0)
	{
		fprintf(stderr, "%s(): pthread_mutex_destroy: %d\n", __FUNCTION__, r);
		return -1;
	}
	vector_destroy(helper.queue);
	memset(&helper, 0, sizeof(helper));
	return 0;
}

#define mutex_lock(m) \
	do { \
		int r = pthread_mutex_lock(m); \
		if (r < 0) { \
			fprintf(stderr, "%s():%d: pthread_mutex_lock: %d\n", __FUNCTION__, __LINE__, r); \
			assert(0); \
		} \
	} while(0)

#define mutex_unlock(m) \
	do { \
		int r = pthread_mutex_unlock(m); \
		if (r < 0) { \
			fprintf(stderr, "%s():%d: pthread_mutex_unlock: %d\n", __FUNCTION__, __LINE__, r); \
			assert(0); \
		} \
	} while(0)

static void * helper_thread(void * ignore)
{
	while (1)
	{
		queue_entry_t * qe;
		char b = 1;

		mutex_lock(&helper.mutex);
		if (!vector_size(helper.queue))
		{
			helper.alive = 0;
			mutex_unlock(&helper.mutex);
			Dprintf("%s() EXIT\n", __FUNCTION__);
			return NULL;
		}
		qe = vector_elt(helper.queue, 0);
		// NOTE: vector_erase() is O(|helper.queue|). If this queue
		// gets to be big we can change how this removal works.
		vector_erase(helper.queue, 0);
		mutex_unlock(&helper.mutex);

		if (qe->action == QEMOUNT)
			helper_thread_mount(qe->mount);
		else if (qe->action == QEUNMOUNT)
		{
			helper_thread_unmount(qe->mount);
			if (write(unmount_pipe[1], &b, 1) != 1)
				perror("helper_thread: write");
		}
		memset(qe, 0, sizeof(*qe));
		free(qe);
	}
	assert(0); // not reachable
	return NULL; // placate compiler
}


// Functions used to interact with helper_thread from non-thread-aware
// fuse_serve_mount code

int fuse_serve_mount_start_shutdown(void)
{
	char b = 1;
	int i = 0;
	bool failed_found;
	Dprintf("%s()\n", __FUNCTION__);

	if (shutdown_has_started())
		return -1;

	helper.shutdown_started = 1;

	// NOTE: we can probably update this and helper_thread's code
	// so that calling this function shortly after an add or remove is
	// safe.
	while (helper.alive)
	{
		if (++i > 4*MAX_START_SHUTDOWN_WAIT)
		{
			fprintf(stderr, "%s(): Mounts or unmounts still in progress. Good luck with the shutdown!\n", __FUNCTION__);
			break;
		}
		jsleep(HZ / 4);
	}

	// Purge failed mounts
	do {
		mount_t ** mp;
		failed_found = 0;
		for (mp = mounts; mp && *mp; mp++)
			if (!(*mp)->mounted) {
				mount_t * m = *mp;
				failed_found = 1;
				mounts_remove(m);
				free(m->kfs_path);
				fuse_opt_free_args(&m->args);
				free(m->mountpoint);
				hash_map_destroy(m->parents);
				memset(m, 0, sizeof(*m));
				free(m);
				break;
			}
	} while (failed_found);

	// If only root is mounted unmount it and return shutdown
	if (nmounts == 1)
		return unmount_root();

	// Start the calling of fuse_serve_mount_step_shutdown()
	if (write(unmount_pipe[1], &b, 1) != 1)
	{
		perror("fuse_serve_mount_start_shutdown(): write");
		helper.shutdown_started = 0;
		return -1;
	}

	return 0;
}


static int enqueue_helper_request(queue_entry_t * qe)
{
	int r;
	Dprintf("%s(%d, \"%s\")\n", __FUNCTION__, qe->action, qe->mount->kfs_path);
	mutex_lock(&helper.mutex);
	r = vector_push_back(helper.queue, qe);
	mutex_unlock(&helper.mutex);
	return r;
}

static int ensure_helper_is_running(void)
{
	pthread_t thread;
	int r;

	if (!root_service_started)
		return 0;

	mutex_lock(&helper.mutex);
	if (helper.alive)
	{
		mutex_unlock(&helper.mutex);
		return 0;
	}
	mutex_unlock(&helper.mutex);

	helper.alive = 1;
	if ((r = pthread_create(&thread, NULL, &helper_thread, NULL)))
	{
		fprintf(stderr, "%s: pthread_create: %d\n", __FUNCTION__, r);
		return -1;
	}
	if ((r = pthread_detach(thread)) && errno != ESRCH)
	{
		fprintf(stderr, "%s: pthread_detach: %d\n", __FUNCTION__, r);
		return -1;
	}

	return 0;
}

static bool shutdown_has_started(void)
{
	return helper.shutdown_started;
}
