/*   SPDX-License-Identifier: BSD-3-Clause
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/file.h"
#include "spdk/fsdev.h"
#include "spdk/init.h"
#include "spdk/log.h"
#include "spdk/queue.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"

#include "spdk_internal/event.h"

#include "config-host.h"
#include "fio.h"
#include "optgroup.h"

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

#define SPDK_FIO_POLLING_TIMEOUT 1000000000ULL
#define SPDK_FIO_DEFAULT_CREATE_MODE (S_IFREG | 0644)

struct spdk_fio_options {
	void *pad;
	char *conf;
	char *json_conf;
	char *env_context;
	char *log_flags;
	unsigned mem_mb;
	int mem_single_seg;
	char *rpc_listen_addr;
	char *fsdev_name;
	unsigned fsdev_mount_max_write;
	int fsdev_writeback_cache;
	unsigned fsdev_create_mode;
};

struct spdk_fio_target {
	struct fio_file			*f;
	struct spdk_fsdev_desc		*desc;
	struct spdk_io_channel		*ch;
	struct spdk_fsdev_file_object	*root;
	struct spdk_fsdev_file_object	*file;
	struct spdk_fsdev_file_handle	*fhandle;
	uint64_t			unique;
	bool				mounted;

	TAILQ_ENTRY(spdk_fio_target)	link;
};

struct spdk_fio_request {
	struct io_u			*io;
	struct thread_data		*td;
	struct iovec			iov;
	struct spdk_fsdev_io_opts	opts;
};

struct spdk_fio_thread {
	struct thread_data		*td;
	struct spdk_thread		*thread;

	TAILQ_HEAD(, spdk_fio_target)	targets;
	bool				failed;
	bool				init_done;

	struct io_u			**iocq;
	unsigned int			iocq_count;
	unsigned int			iocq_size;

	TAILQ_ENTRY(spdk_fio_thread)	link;
};

struct spdk_fio_oat_ctx {
	union {
		struct spdk_fio_setup_args {
			struct thread_data *td;
		} sa;
	} u;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int ret;
};

struct spdk_fio_setup_ctx {
	struct spdk_fio_oat_ctx		*oat;
	struct thread_data		*td;
	struct spdk_fsdev_desc		*desc;
	struct spdk_io_channel		*ch;
	struct spdk_fsdev_file_object	*root;
	struct spdk_fsdev_file_object	*file;
	struct spdk_fsdev_file_handle	*fhandle;
	struct fio_file			*f;
	unsigned int			file_index;
	uint64_t			unique;
	uint64_t			new_file_size;
	int				status;
};

struct spdk_fio_open_ctx {
	struct spdk_fio_thread		*fio_thread;
	struct thread_data		*td;
	struct fio_file			*f;
	struct spdk_fio_target		*target;
	unsigned int			file_index;
	uint64_t			unique;
	int				status;
};

struct spdk_fio_close_ctx {
	struct spdk_fio_target		*target;
	uint64_t			unique;
};

static bool g_spdk_env_initialized = false;
static const char *g_json_config_file = NULL;
static void *g_json_data;
static size_t g_json_data_size;
static const char *g_rpc_listen_addr = NULL;

static pthread_t g_init_thread_id = 0;
static pthread_mutex_t g_init_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_init_cond;
static bool g_poll_loop = true;
static TAILQ_HEAD(, spdk_fio_thread) g_threads = TAILQ_HEAD_INITIALIZER(g_threads);
static pthread_mutex_t g_open_barrier_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_open_barrier_cond = PTHREAD_COND_INITIALIZER;
static int g_open_barrier_group = -1;
static unsigned int g_open_barrier_count;
static unsigned int g_open_barrier_expected;
static bool g_open_barrier_failed;

static __thread bool g_internal_thread = false;

static unsigned int
spdk_fio_open_barrier_expected(struct thread_data *td)
{
	unsigned int count = 0;

	for_each_td(iter) {
		if (iter->groupid == td->groupid && iter->io_ops != NULL &&
		    strcmp(iter->io_ops->name, "spdk_fsdev") == 0) {
			count++;
		}
	} end_for_each();

	return spdk_max(count, 1U);
}

static int
spdk_fio_wait_for_open_barrier(struct thread_data *td, bool failed)
{
	int rc = 0;

	pthread_mutex_lock(&g_open_barrier_mtx);
	if (g_open_barrier_group != (int)td->groupid) {
		g_open_barrier_group = (int)td->groupid;
		g_open_barrier_count = 0;
		g_open_barrier_expected = spdk_fio_open_barrier_expected(td);
		g_open_barrier_failed = false;
	}

	if (failed) {
		g_open_barrier_failed = true;
	}
	g_open_barrier_count++;

	if (g_open_barrier_failed || g_open_barrier_count >= g_open_barrier_expected) {
		pthread_cond_broadcast(&g_open_barrier_cond);
	} else {
		while (!g_open_barrier_failed &&
		       g_open_barrier_count < g_open_barrier_expected) {
			pthread_cond_wait(&g_open_barrier_cond, &g_open_barrier_mtx);
		}
	}

	if (g_open_barrier_failed) {
		rc = -1;
	}
	pthread_mutex_unlock(&g_open_barrier_mtx);

	return rc;
}

static int spdk_fio_init(struct thread_data *td);
static void spdk_fio_cleanup(struct thread_data *td);
static size_t spdk_fio_poll_thread(struct spdk_fio_thread *fio_thread);
static void spdk_fio_setup_oat(void *ctx);
static void spdk_fio_fsdev_event_cb(enum spdk_fsdev_event_type type,
				    struct spdk_fsdev *fsdev, void *event_ctx);

static int
spdk_fio_status_to_errno(int status)
{
	return status < 0 ? -status : status;
}

static uint64_t
spdk_fio_get_requested_file_size(struct thread_data *td, struct fio_file *f)
{
	if (td->o.file_size_high) {
		return td->o.file_size_high;
	}
	if (td->o.file_size_low) {
		return td->o.file_size_low;
	}
	if (td->o.size) {
		return td->o.size / spdk_max(td->o.nr_files, 1U);
	}
	if (f->io_size && f->io_size != UINT64_MAX) {
		return f->io_size;
	}

	return 0;
}

static bool
spdk_fio_valid_file_name(const char *name)
{
	return name != NULL && name[0] != '\0' && strchr(name, '/') == NULL;
}

static mode_t
spdk_fio_get_create_mode(const struct spdk_fio_options *fio_options)
{
	return fio_options->fsdev_create_mode ?
	       fio_options->fsdev_create_mode : SPDK_FIO_DEFAULT_CREATE_MODE;
}

static void
spdk_fio_wake_oat_waiter(struct spdk_fio_oat_ctx *ctx)
{
	pthread_mutex_lock(&ctx->mutex);
	pthread_cond_signal(&ctx->cond);
	pthread_mutex_unlock(&ctx->mutex);
}

static void
spdk_fio_sync_run_oat(void (*msg_fn)(void *), struct spdk_fio_oat_ctx *ctx)
{
	assert(!spdk_thread_is_app_thread(NULL));

	pthread_mutex_init(&ctx->mutex, NULL);
	pthread_cond_init(&ctx->cond, NULL);
	pthread_mutex_lock(&ctx->mutex);

	spdk_thread_send_msg(spdk_thread_get_app_thread(), msg_fn, ctx);

	pthread_mutex_lock(&g_init_mtx);
	pthread_cond_signal(&g_init_cond);
	pthread_mutex_unlock(&g_init_mtx);

	pthread_cond_wait(&ctx->cond, &ctx->mutex);
	pthread_mutex_unlock(&ctx->mutex);

	pthread_mutex_destroy(&ctx->mutex);
	pthread_cond_destroy(&ctx->cond);
}

static int
spdk_fio_schedule_thread(struct spdk_thread *thread)
{
	struct spdk_fio_thread *fio_thread;

	if (g_internal_thread) {
		return 0;
	}

	fio_thread = spdk_thread_get_ctx(thread);

	pthread_mutex_lock(&g_init_mtx);
	TAILQ_INSERT_TAIL(&g_threads, fio_thread, link);
	pthread_mutex_unlock(&g_init_mtx);

	return 0;
}

static int
spdk_fio_init_thread(struct thread_data *td)
{
	struct spdk_fio_thread *fio_thread;
	struct spdk_thread *thread;

	g_internal_thread = true;
	thread = spdk_thread_create("fio_fsdev_thread", NULL);
	g_internal_thread = false;
	if (!thread) {
		SPDK_ERRLOG("failed to allocate thread\n");
		return -1;
	}

	fio_thread = spdk_thread_get_ctx(thread);
	fio_thread->td = td;
	fio_thread->thread = thread;
	td->io_ops_data = fio_thread;

	spdk_set_thread(thread);

	fio_thread->iocq_size = td->o.iodepth;
	fio_thread->iocq = calloc(fio_thread->iocq_size, sizeof(struct io_u *));
	if (fio_thread->iocq == NULL) {
		return -1;
	}

	TAILQ_INIT(&fio_thread->targets);

	return 0;
}

static void spdk_fio_close_target_continue(struct spdk_fio_close_ctx *ctx);

static void
spdk_fio_close_target_done(struct spdk_fio_close_ctx *ctx)
{
	struct spdk_fio_target *target = ctx->target;

	if (target->ch != NULL) {
		spdk_put_io_channel(target->ch);
	}
	if (target->desc != NULL) {
		spdk_fsdev_close(target->desc);
	}

	free(target);
	free(ctx);
}

static void
spdk_fio_close_umount_complete(void *cb_arg, struct spdk_io_channel *ch)
{
	struct spdk_fio_close_ctx *ctx = cb_arg;

	ctx->target->mounted = false;
	spdk_fio_close_target_done(ctx);
}

static void
spdk_fio_close_forget_complete(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct spdk_fio_close_ctx *ctx = cb_arg;

	ctx->target->file = NULL;
	spdk_fio_close_target_continue(ctx);
}

static void
spdk_fio_close_release_complete(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct spdk_fio_close_ctx *ctx = cb_arg;

	ctx->target->fhandle = NULL;
	spdk_fio_close_target_continue(ctx);
}

static void
spdk_fio_close_target_continue(struct spdk_fio_close_ctx *ctx)
{
	struct spdk_fio_target *target = ctx->target;
	int rc;

	if (target->fhandle != NULL) {
		rc = spdk_fsdev_release(target->desc, target->ch, ctx->unique++, target->file,
					target->fhandle, spdk_fio_close_release_complete, ctx);
		if (rc == 0) {
			return;
		}
		target->fhandle = NULL;
	}

	if (target->file != NULL) {
		rc = spdk_fsdev_forget(target->desc, target->ch, ctx->unique++, target->file, 1,
				       spdk_fio_close_forget_complete, ctx);
		if (rc == 0) {
			return;
		}
		target->file = NULL;
	}

	if (target->mounted) {
		rc = spdk_fsdev_umount(target->desc, target->ch, ctx->unique++,
				       spdk_fio_close_umount_complete, ctx);
		if (rc == 0) {
			return;
		}
		target->mounted = false;
	}

	spdk_fio_close_target_done(ctx);
}

static void
spdk_fio_fsdev_close_targets(void *arg)
{
	struct spdk_fio_thread *fio_thread = arg;
	struct spdk_fio_target *target, *tmp;
	struct spdk_fio_close_ctx *ctx;

	TAILQ_FOREACH_SAFE(target, &fio_thread->targets, link, tmp) {
		TAILQ_REMOVE(&fio_thread->targets, target, link);
		if (target->f != NULL) {
			target->f->engine_data = NULL;
			target->f = NULL;
		}

		ctx = calloc(1, sizeof(*ctx));
		if (ctx == NULL) {
			if (target->ch != NULL) {
				spdk_put_io_channel(target->ch);
			}
			if (target->desc != NULL) {
				spdk_fsdev_close(target->desc);
			}
			free(target);
			continue;
		}

		ctx->target = target;
		ctx->unique = 1;
		spdk_fio_close_target_continue(ctx);
	}
}

static void
spdk_fio_detach_target_files(struct spdk_fio_thread *fio_thread)
{
	struct spdk_fio_target *target;

	TAILQ_FOREACH(target, &fio_thread->targets, link) {
		if (target->f != NULL) {
			target->f->engine_data = NULL;
			target->f = NULL;
		}
	}
}

static void
spdk_fio_cleanup_thread(struct spdk_fio_thread *fio_thread)
{
	spdk_thread_send_msg(fio_thread->thread, spdk_fio_fsdev_close_targets, fio_thread);

	pthread_mutex_lock(&g_init_mtx);
	TAILQ_INSERT_TAIL(&g_threads, fio_thread, link);
	pthread_mutex_unlock(&g_init_mtx);
}

static void
spdk_fio_calc_timeout(struct spdk_fio_thread *fio_thread, struct timespec *ts)
{
	uint64_t timeout, now;

	if (spdk_thread_has_active_pollers(fio_thread->thread)) {
		return;
	}

	timeout = spdk_thread_next_poller_expiration(fio_thread->thread);
	now = spdk_get_ticks();

	if (timeout == 0) {
		timeout = now + (SPDK_FIO_POLLING_TIMEOUT * spdk_get_ticks_hz()) / SPDK_SEC_TO_NSEC;
	}

	if (timeout > now) {
		timeout = ((timeout - now) * SPDK_SEC_TO_NSEC) / spdk_get_ticks_hz() +
			  ts->tv_sec * SPDK_SEC_TO_NSEC + ts->tv_nsec;

		ts->tv_sec  = timeout / SPDK_SEC_TO_NSEC;
		ts->tv_nsec = timeout % SPDK_SEC_TO_NSEC;
	}
}

static void
spdk_fio_fsdev_init_done(int rc, void *cb_arg)
{
	*(bool *)cb_arg = true;

	free(g_json_data);
	if (rc) {
		SPDK_ERRLOG("RUNTIME RPCs failed\n");
		exit(1);
	}
}

static void
spdk_fio_fsdev_subsystem_init_done(int rc, void *cb_arg)
{
	if (rc) {
		SPDK_ERRLOG("subsystem init failed\n");
		exit(1);
	}

	spdk_rpc_set_state(SPDK_RPC_RUNTIME);
	spdk_subsystem_load_config(g_json_data, g_json_data_size,
				   spdk_fio_fsdev_init_done, cb_arg, true);
}

static void
spdk_fio_fsdev_startup_done(int rc, void *cb_arg)
{
	if (rc) {
		SPDK_ERRLOG("STARTUP RPCs failed\n");
		exit(1);
	}

	if (g_rpc_listen_addr != NULL) {
		if (spdk_rpc_initialize(g_rpc_listen_addr, NULL) != 0) {
			SPDK_ERRLOG("could not initialize RPC address %s\n", g_rpc_listen_addr);
			exit(1);
		}
	}

	spdk_subsystem_init(spdk_fio_fsdev_subsystem_init_done, cb_arg);
}

static void
spdk_fio_fsdev_init_start(void *arg)
{
	bool *done = arg;

	g_json_data = spdk_posix_file_load_from_name(g_json_config_file, &g_json_data_size);
	if (g_json_data == NULL) {
		SPDK_ERRLOG("could not allocate buffer for json config file\n");
		exit(1);
	}

	assert(spdk_rpc_get_state() == SPDK_RPC_STARTUP);
	spdk_subsystem_load_config(g_json_data, g_json_data_size,
				   spdk_fio_fsdev_startup_done, done, true);
}

static void
spdk_fio_fsdev_fini_done(void *cb_arg)
{
	*(bool *)cb_arg = true;

	spdk_rpc_finish();
}

static void
spdk_fio_fsdev_fini_start(void *arg)
{
	bool *done = arg;

	spdk_subsystem_fini(spdk_fio_fsdev_fini_done, done);
}

static void *
spdk_init_thread_poll(void *arg)
{
	struct spdk_fio_options		*eo = arg;
	struct spdk_fio_thread		*fio_thread;
	struct spdk_fio_thread		*thread, *tmp;
	struct spdk_env_opts		opts;
	bool				done;
	int				rc;
	struct timespec			ts;
	struct thread_data		td = {};

	td.o.iodepth = 32;
	td.eo = eo;

	if (eo->conf && eo->json_conf) {
		SPDK_ERRLOG("Cannot provide two types of configuration files\n");
		rc = EINVAL;
		goto err_exit;
	} else if (eo->conf && strlen(eo->conf)) {
		g_json_config_file = eo->conf;
	} else if (eo->json_conf && strlen(eo->json_conf)) {
		g_json_config_file = eo->json_conf;
	} else {
		SPDK_ERRLOG("No configuration file provided\n");
		rc = EINVAL;
		goto err_exit;
	}

	if (eo->rpc_listen_addr) {
		g_rpc_listen_addr = eo->rpc_listen_addr;
	}

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "fio";
	if (eo->mem_mb) {
		opts.mem_size = eo->mem_mb;
	}
	opts.hugepage_single_segments = eo->mem_single_seg;
	if (eo->env_context) {
		opts.env_context = eo->env_context;
	}

	if (spdk_env_init(&opts) < 0) {
		SPDK_ERRLOG("Unable to initialize SPDK env\n");
		rc = EINVAL;
		goto err_exit;
	}
	spdk_unaffinitize_thread();

	if (eo->log_flags) {
		char *sp = NULL;
		char *tok = strtok_r(eo->log_flags, ",", &sp);
		do {
			rc = spdk_log_set_flag(tok);
			if (rc < 0) {
				SPDK_ERRLOG("unknown spdk log flag %s\n", tok);
				rc = EINVAL;
				goto err_exit;
			}
		} while ((tok = strtok_r(NULL, ",", &sp)) != NULL);
#ifdef DEBUG
		spdk_log_set_print_level(SPDK_LOG_DEBUG);
#endif
	}

	spdk_thread_lib_init(spdk_fio_schedule_thread, sizeof(struct spdk_fio_thread));

	rc = spdk_fio_init_thread(&td);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create initialization thread\n");
		goto err_exit;
	}

	fio_thread = td.io_ops_data;

	done = false;
	spdk_thread_send_msg(fio_thread->thread, spdk_fio_fsdev_init_start, &done);
	do {
		spdk_fio_poll_thread(fio_thread);
	} while (!done);

	while (spdk_fio_poll_thread(fio_thread) > 0) {};

	pthread_mutex_lock(&g_init_mtx);
	pthread_cond_signal(&g_init_cond);
	pthread_mutex_unlock(&g_init_mtx);

	while (g_poll_loop) {
		spdk_fio_poll_thread(fio_thread);

		pthread_mutex_lock(&g_init_mtx);
		if (!TAILQ_EMPTY(&g_threads)) {
			TAILQ_FOREACH_SAFE(thread, &g_threads, link, tmp) {
				if (spdk_thread_is_exited(thread->thread)) {
					TAILQ_REMOVE(&g_threads, thread, link);
					free(thread->iocq);
					spdk_thread_destroy(thread->thread);
				} else {
					spdk_fio_poll_thread(thread);
				}
			}

			pthread_mutex_unlock(&g_init_mtx);
			continue;
		}

		clock_gettime(CLOCK_MONOTONIC, &ts);
		spdk_fio_calc_timeout(fio_thread, &ts);

		rc = pthread_cond_timedwait(&g_init_cond, &g_init_mtx, &ts);
		pthread_mutex_unlock(&g_init_mtx);

		if (rc != 0 && rc != ETIMEDOUT) {
			break;
		}
	}

	spdk_fio_cleanup_thread(fio_thread);

	done = false;
	spdk_thread_send_msg(fio_thread->thread, spdk_fio_fsdev_fini_start, &done);
	do {
		spdk_fio_poll_thread(fio_thread);
		TAILQ_FOREACH_SAFE(thread, &g_threads, link, tmp) {
			spdk_fio_poll_thread(thread);
		}
	} while (!done);

	TAILQ_FOREACH(thread, &g_threads, link) {
		spdk_set_thread(thread->thread);
		spdk_thread_exit(thread->thread);
		spdk_set_thread(NULL);
	}

	while (!TAILQ_EMPTY(&g_threads)) {
		TAILQ_FOREACH_SAFE(thread, &g_threads, link, tmp) {
			if (spdk_thread_is_exited(thread->thread)) {
				TAILQ_REMOVE(&g_threads, thread, link);
				free(thread->iocq);
				spdk_thread_destroy(thread->thread);
			} else {
				spdk_thread_poll(thread->thread, 0, 0);
			}
		}
	}

	pthread_exit(NULL);

err_exit:
	exit(rc);
	return NULL;
}

static int
spdk_fio_init_env(struct thread_data *td)
{
	pthread_condattr_t attr;
	int rc = -1;

	if (pthread_condattr_init(&attr)) {
		SPDK_ERRLOG("Unable to initialize condition variable\n");
		return -1;
	}

	if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) {
		SPDK_ERRLOG("Unable to initialize condition variable\n");
		goto out;
	}

	if (pthread_cond_init(&g_init_cond, &attr)) {
		SPDK_ERRLOG("Unable to initialize condition variable\n");
		goto out;
	}

	rc = pthread_create(&g_init_thread_id, NULL, &spdk_init_thread_poll, td->eo);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to spawn thread to poll fsdev completions.\n");
	}

	pthread_mutex_lock(&g_init_mtx);
	pthread_cond_wait(&g_init_cond, &g_init_mtx);
	pthread_mutex_unlock(&g_init_mtx);
out:
	pthread_condattr_destroy(&attr);
	return rc;
}

static bool
fio_redirected_to_dev_null(void)
{
	char path[PATH_MAX] = "";
	ssize_t ret;

	ret = readlink("/proc/self/fd/1", path, sizeof(path));
	if (ret == -1 || strcmp(path, "/dev/null") != 0) {
		return false;
	}

	ret = readlink("/proc/self/fd/2", path, sizeof(path));
	if (ret == -1 || strcmp(path, "/dev/null") != 0) {
		return false;
	}

	return true;
}

static int
spdk_fio_init_spdk_env(struct thread_data *td)
{
	static pthread_mutex_t setup_lock = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&setup_lock);
	if (!g_spdk_env_initialized) {
		if (spdk_fio_init_env(td)) {
			pthread_mutex_unlock(&setup_lock);
			SPDK_ERRLOG("failed to initialize\n");
			return -1;
		}

		g_spdk_env_initialized = true;
	}
	pthread_mutex_unlock(&setup_lock);

	return 0;
}

static void spdk_fio_setup_next_file(struct spdk_fio_setup_ctx *setup);
static void spdk_fio_setup_finish(struct spdk_fio_setup_ctx *setup);

static void
spdk_fio_setup_umount_complete(void *cb_arg, struct spdk_io_channel *ch)
{
	struct spdk_fio_setup_ctx *setup = cb_arg;

	setup->root = NULL;
	spdk_fio_setup_finish(setup);
}

static void
spdk_fio_setup_forget_complete(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct spdk_fio_setup_ctx *setup = cb_arg;

	setup->file = NULL;
	spdk_fio_setup_next_file(setup);
}

static void
spdk_fio_setup_release_complete(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct spdk_fio_setup_ctx *setup = cb_arg;
	int rc;

	setup->fhandle = NULL;
	rc = spdk_fsdev_forget(setup->desc, setup->ch, setup->unique++, setup->file, 1,
			       spdk_fio_setup_forget_complete, setup);
	if (rc != 0) {
		setup->status = rc;
		spdk_fio_setup_finish(setup);
	}
}

static void
spdk_fio_setup_create_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
			       struct spdk_fsdev_file_object *fobject,
			       const struct spdk_fsdev_file_attr *attr,
			       struct spdk_fsdev_file_handle *fhandle)
{
	struct spdk_fio_setup_ctx *setup = cb_arg;
	int rc;

	if (status != 0) {
		setup->status = status;
		spdk_fio_setup_finish(setup);
		return;
	}

	setup->f->real_file_size = setup->new_file_size;
	setup->f->filetype = FIO_TYPE_BLOCK;
	fio_file_set_size_known(setup->f);

	setup->file = fobject;
	setup->fhandle = fhandle;
	rc = spdk_fsdev_release(setup->desc, setup->ch, setup->unique++, setup->file,
				setup->fhandle, spdk_fio_setup_release_complete, setup);
	if (rc != 0) {
		setup->status = rc;
		spdk_fio_setup_finish(setup);
	}
}

static void
spdk_fio_setup_lookup_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
			       struct spdk_fsdev_file_object *fobject,
			       const struct spdk_fsdev_file_attr *attr)
{
	struct spdk_fio_setup_ctx *setup = cb_arg;
	struct spdk_fio_options *fio_options = setup->td->eo;
	int rc;

	if (status == -ENOENT) {
		setup->new_file_size = spdk_fio_get_requested_file_size(setup->td, setup->f);
		if (setup->new_file_size == 0) {
			SPDK_ERRLOG("file %s does not exist; size/filesize is required for create\n",
				    setup->f->file_name);
			setup->status = -EINVAL;
			spdk_fio_setup_finish(setup);
			return;
		}

		rc = spdk_fsdev_create(setup->desc, setup->ch, setup->unique++, setup->root,
				       setup->f->file_name, spdk_fio_get_create_mode(fio_options),
				       O_RDWR, 0, 0, 0, spdk_fio_setup_create_complete, setup);
		if (rc != 0) {
			setup->status = rc;
			spdk_fio_setup_finish(setup);
		}
		return;
	}

	if (status != 0) {
		setup->status = status;
		spdk_fio_setup_finish(setup);
		return;
	}

	/*
	 * FSSSD lookup currently returns an inode-only attr, so size can be 0 even
	 * when fio has an explicit size/filesize configured for the target.
	 * TODO: Change NFS_LOOKUP to get response with attr.
	 */
	setup->new_file_size = spdk_fio_get_requested_file_size(setup->td, setup->f);
	setup->f->real_file_size = attr->size != 0 ? attr->size : setup->new_file_size;
	setup->f->filetype = FIO_TYPE_BLOCK;
	fio_file_set_size_known(setup->f);

	setup->file = fobject;
	rc = spdk_fsdev_forget(setup->desc, setup->ch, setup->unique++, setup->file, 1,
			       spdk_fio_setup_forget_complete, setup);
	if (rc != 0) {
		setup->status = rc;
		spdk_fio_setup_finish(setup);
	}
}

static void
spdk_fio_setup_next_file(struct spdk_fio_setup_ctx *setup)
{
	struct fio_file *f;
	unsigned int i;
	int rc;

	setup->file = NULL;
	setup->fhandle = NULL;
	setup->f = NULL;

	for_each_file(setup->td, f, i) {
		if (i < setup->file_index) {
			continue;
		}

		setup->file_index = i + 1;
		setup->f = f;
		if (!spdk_fio_valid_file_name(f->file_name)) {
			SPDK_ERRLOG("filename must be a single root entry: %s\n", f->file_name);
			setup->status = -EINVAL;
			spdk_fio_setup_finish(setup);
			return;
		}

		rc = spdk_fsdev_lookup(setup->desc, setup->ch, setup->unique++, setup->root,
				       f->file_name, spdk_fio_setup_lookup_complete, setup);
		if (rc != 0) {
			setup->status = rc;
			spdk_fio_setup_finish(setup);
		}
		return;
	}

	spdk_fio_setup_finish(setup);
}

static void
spdk_fio_setup_mount_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
			      const struct spdk_fsdev_mount_opts *opts,
			      struct spdk_fsdev_file_object *root_fobject)
{
	struct spdk_fio_setup_ctx *setup = cb_arg;

	if (status != 0) {
		setup->status = status;
		spdk_fio_setup_finish(setup);
		return;
	}

	setup->root = root_fobject;
	spdk_fio_setup_next_file(setup);
}

static void
spdk_fio_setup_finish(struct spdk_fio_setup_ctx *setup)
{
	struct spdk_fio_oat_ctx *oat = setup->oat;
	int rc;

	if (setup->root != NULL) {
		rc = spdk_fsdev_umount(setup->desc, setup->ch, setup->unique++,
				       spdk_fio_setup_umount_complete, setup);
		if (rc == 0) {
			return;
		}
		setup->root = NULL;
		if (setup->status == 0) {
			setup->status = rc;
		}
	}

	if (setup->ch != NULL) {
		spdk_put_io_channel(setup->ch);
	}
	if (setup->desc != NULL) {
		spdk_fsdev_close(setup->desc);
	}

	oat->ret = setup->status == 0 ? 0 : -1;
	free(setup);
	spdk_fio_wake_oat_waiter(oat);
}

static void
spdk_fio_setup_oat(void *_ctx)
{
	struct spdk_fio_oat_ctx *oat = _ctx;
	struct thread_data *td = oat->u.sa.td;
	struct spdk_fio_options *fio_options = td->eo;
	struct spdk_fio_setup_ctx *setup;
	struct spdk_fsdev_mount_opts mount_opts = {};
	int rc;

	if (fio_options->fsdev_name == NULL || fio_options->fsdev_name[0] == '\0') {
		SPDK_ERRLOG("fsdev_name is required\n");
		oat->ret = -1;
		spdk_fio_wake_oat_waiter(oat);
		return;
	}

	setup = calloc(1, sizeof(*setup));
	if (setup == NULL) {
		oat->ret = -1;
		spdk_fio_wake_oat_waiter(oat);
		return;
	}

	setup->oat = oat;
	setup->td = td;
	setup->unique = 1;

	rc = spdk_fsdev_open(fio_options->fsdev_name, spdk_fio_fsdev_event_cb, NULL, &setup->desc);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to open fsdev %s\n", fio_options->fsdev_name);
		setup->status = rc;
		spdk_fio_setup_finish(setup);
		return;
	}

	setup->ch = spdk_fsdev_get_io_channel(setup->desc);
	if (setup->ch == NULL) {
		setup->status = -ENOMEM;
		spdk_fio_setup_finish(setup);
		return;
	}

	mount_opts.opts_size = sizeof(mount_opts);
	mount_opts.max_write = fio_options->fsdev_mount_max_write ?
			       fio_options->fsdev_mount_max_write : UINT32_MAX;
	mount_opts.writeback_cache_enabled = fio_options->fsdev_writeback_cache;

	rc = spdk_fsdev_mount(setup->desc, setup->ch, setup->unique++, &mount_opts,
			      spdk_fio_setup_mount_complete, setup);
	if (rc != 0) {
		setup->status = rc;
		spdk_fio_setup_finish(setup);
	}
}

static int
spdk_fio_setup(struct thread_data *td)
{
	struct spdk_fio_oat_ctx ctx = { 0 };

	if (is_backend && !fio_redirected_to_dev_null()) {
		char buf[1024];
		snprintf(buf, sizeof(buf),
			 "SPDK fsdev FIO plugin is in daemon mode, but stdout/stderr "
			 "aren't redirected to /dev/null. Aborting.");
		fio_server_text_output(FIO_LOG_ERR, buf, sizeof(buf));
		return -1;
	}

	if (!td->o.use_thread) {
		SPDK_ERRLOG("must set thread=1 when using spdk plugin\n");
		return -1;
	}

	if (spdk_fio_init_spdk_env(td) != 0) {
		return -1;
	}

	ctx.u.sa.td = td;
	spdk_fio_sync_run_oat(spdk_fio_setup_oat, &ctx);
	return ctx.ret;
}

static void spdk_fio_open_next_file(struct spdk_fio_open_ctx *open_ctx);
static void spdk_fio_open_complete_all(struct spdk_fio_open_ctx *open_ctx);

static void
spdk_fio_fsdev_event_cb(enum spdk_fsdev_event_type type, struct spdk_fsdev *fsdev,
			void *event_ctx)
{
	SPDK_WARNLOG("Unsupported fsdev event: type %d\n", type);
}

static void
spdk_fio_open_fopen_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
			     struct spdk_fsdev_file_handle *fhandle)
{
	struct spdk_fio_open_ctx *open_ctx = cb_arg;

	if (status != 0) {
		open_ctx->status = status;
		spdk_fio_open_complete_all(open_ctx);
		return;
	}

	open_ctx->target->fhandle = fhandle;
	open_ctx->f->engine_data = open_ctx->target;
	TAILQ_INSERT_TAIL(&open_ctx->fio_thread->targets, open_ctx->target, link);
	open_ctx->target = NULL;
	spdk_fio_open_next_file(open_ctx);
}

static void
spdk_fio_open_create_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
			      struct spdk_fsdev_file_object *fobject,
			      const struct spdk_fsdev_file_attr *attr,
			      struct spdk_fsdev_file_handle *fhandle)
{
	struct spdk_fio_open_ctx *open_ctx = cb_arg;

	if (status != 0) {
		open_ctx->status = status;
		spdk_fio_open_complete_all(open_ctx);
		return;
	}

	open_ctx->target->file = fobject;
	open_ctx->target->fhandle = fhandle;
	open_ctx->f->engine_data = open_ctx->target;
	TAILQ_INSERT_TAIL(&open_ctx->fio_thread->targets, open_ctx->target, link);
	open_ctx->target = NULL;
	spdk_fio_open_next_file(open_ctx);
}

static void
spdk_fio_open_lookup_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
			      struct spdk_fsdev_file_object *fobject,
			      const struct spdk_fsdev_file_attr *attr)
{
	struct spdk_fio_open_ctx *open_ctx = cb_arg;
	struct spdk_fio_options *fio_options = open_ctx->td->eo;
	int rc;

	if (status == -ENOENT) {
		rc = spdk_fsdev_create(open_ctx->target->desc, open_ctx->target->ch,
				       open_ctx->unique++, open_ctx->target->root,
				       open_ctx->f->file_name, spdk_fio_get_create_mode(fio_options),
				       O_RDWR, 0, 0, 0, spdk_fio_open_create_complete, open_ctx);
		if (rc != 0) {
			open_ctx->status = rc;
			spdk_fio_open_complete_all(open_ctx);
		}
		return;
	}

	if (status != 0) {
		open_ctx->status = status;
		spdk_fio_open_complete_all(open_ctx);
		return;
	}

	open_ctx->target->file = fobject;
	rc = spdk_fsdev_fopen(open_ctx->target->desc, open_ctx->target->ch, open_ctx->unique++,
			      open_ctx->target->file, O_RDWR, spdk_fio_open_fopen_complete, open_ctx);
	if (rc != 0) {
		open_ctx->status = rc;
		spdk_fio_open_complete_all(open_ctx);
	}
}

static void
spdk_fio_open_mount_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
			     const struct spdk_fsdev_mount_opts *opts,
			     struct spdk_fsdev_file_object *root_fobject)
{
	struct spdk_fio_open_ctx *open_ctx = cb_arg;
	int rc;

	if (status != 0) {
		open_ctx->status = status;
		spdk_fio_open_complete_all(open_ctx);
		return;
	}

	open_ctx->target->root = root_fobject;
	open_ctx->target->mounted = true;
	rc = spdk_fsdev_lookup(open_ctx->target->desc, open_ctx->target->ch, open_ctx->unique++,
			       open_ctx->target->root, open_ctx->f->file_name,
			       spdk_fio_open_lookup_complete, open_ctx);
	if (rc != 0) {
		open_ctx->status = rc;
		spdk_fio_open_complete_all(open_ctx);
	}
}

static void
spdk_fio_open_next_file(struct spdk_fio_open_ctx *open_ctx)
{
	struct spdk_fio_options *fio_options = open_ctx->td->eo;
	struct spdk_fsdev_mount_opts mount_opts = {};
	struct fio_file *f;
	unsigned int i;
	int rc;

	for_each_file(open_ctx->td, f, i) {
		if (i < open_ctx->file_index) {
			continue;
		}

		open_ctx->file_index = i + 1;
		open_ctx->f = f;

		open_ctx->target = calloc(1, sizeof(*open_ctx->target));
		if (open_ctx->target == NULL) {
			open_ctx->status = -ENOMEM;
			spdk_fio_open_complete_all(open_ctx);
			return;
		}

		open_ctx->target->f = f;
		open_ctx->target->unique = 1;
		rc = spdk_fsdev_open(fio_options->fsdev_name, spdk_fio_fsdev_event_cb, NULL,
				     &open_ctx->target->desc);
		if (rc != 0) {
			open_ctx->status = rc;
			spdk_fio_open_complete_all(open_ctx);
			return;
		}

		open_ctx->target->ch = spdk_fsdev_get_io_channel(open_ctx->target->desc);
		if (open_ctx->target->ch == NULL) {
			open_ctx->status = -ENOMEM;
			spdk_fio_open_complete_all(open_ctx);
			return;
		}

		mount_opts.opts_size = sizeof(mount_opts);
		mount_opts.max_write = fio_options->fsdev_mount_max_write ?
				       fio_options->fsdev_mount_max_write : UINT32_MAX;
		mount_opts.writeback_cache_enabled = fio_options->fsdev_writeback_cache;

		rc = spdk_fsdev_mount(open_ctx->target->desc, open_ctx->target->ch,
				      open_ctx->unique++, &mount_opts,
				      spdk_fio_open_mount_complete, open_ctx);
		if (rc != 0) {
			open_ctx->status = rc;
			spdk_fio_open_complete_all(open_ctx);
		}
		return;
	}

	spdk_fio_open_complete_all(open_ctx);
}

static void
spdk_fio_open_complete_all(struct spdk_fio_open_ctx *open_ctx)
{
	if (open_ctx->target != NULL) {
		if (open_ctx->target->ch != NULL) {
			spdk_put_io_channel(open_ctx->target->ch);
		}
		if (open_ctx->target->desc != NULL) {
			spdk_fsdev_close(open_ctx->target->desc);
		}
		free(open_ctx->target);
	}

	if (open_ctx->status != 0) {
		open_ctx->fio_thread->failed = true;
	}
	open_ctx->fio_thread->init_done = true;
	free(open_ctx);
}

static void
spdk_fio_fsdev_open(void *arg)
{
	struct thread_data *td = arg;
	struct spdk_fio_thread *fio_thread = td->io_ops_data;
	struct spdk_fio_open_ctx *open_ctx;

	open_ctx = calloc(1, sizeof(*open_ctx));
	if (open_ctx == NULL) {
		fio_thread->failed = true;
		fio_thread->init_done = true;
		return;
	}

	open_ctx->fio_thread = fio_thread;
	open_ctx->td = td;
	open_ctx->unique = 1;
	spdk_fio_open_next_file(open_ctx);
}

static int
spdk_fio_init(struct thread_data *td)
{
	struct spdk_fio_thread *fio_thread;
	int rc;

	if (spdk_fio_init_spdk_env(td) != 0) {
		return -1;
	}

	if (td->io_ops_data) {
		return 0;
	}

	rc = spdk_fio_init_thread(td);
	if (rc) {
		return rc;
	}

	fio_thread = td->io_ops_data;
	fio_thread->failed = false;
	fio_thread->init_done = false;

	spdk_thread_send_msg(fio_thread->thread, spdk_fio_fsdev_open, td);
	do {
		spdk_fio_poll_thread(fio_thread);
	} while (!fio_thread->init_done);

	if (spdk_fio_wait_for_open_barrier(td, fio_thread->failed) != 0) {
		return -1;
	}

	if (fio_thread->failed) {
		return -1;
	}

	return 0;
}

static void
spdk_fio_cleanup(struct thread_data *td)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;

	spdk_fio_detach_target_files(fio_thread);
	spdk_fio_cleanup_thread(fio_thread);
	td->io_ops_data = NULL;
}

static int
spdk_fio_open(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

static int
spdk_fio_close(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

static int
spdk_fio_iomem_alloc(struct thread_data *td, size_t total_mem)
{
	td->orig_buffer = spdk_dma_zmalloc(total_mem, 0x1000, NULL);
	return td->orig_buffer == NULL;
}

static void
spdk_fio_iomem_free(struct thread_data *td)
{
	spdk_dma_free(td->orig_buffer);
}

static int
spdk_fio_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct spdk_fio_request *fio_req;

	io_u->engine_data = NULL;

	fio_req = calloc(1, sizeof(*fio_req));
	if (fio_req == NULL) {
		return 1;
	}
	fio_req->io = io_u;
	fio_req->td = td;
	fio_req->opts.size = sizeof(fio_req->opts);

	io_u->engine_data = fio_req;

	return 0;
}

static void
spdk_fio_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct spdk_fio_request *fio_req = io_u->engine_data;

	if (fio_req) {
		assert(fio_req->io == io_u);
		free(fio_req);
		io_u->engine_data = NULL;
	}
}

static void
spdk_fio_complete_io(struct spdk_fio_request *fio_req, int status, uint32_t data_size)
{
	struct thread_data *td = fio_req->td;
	struct spdk_fio_thread *fio_thread = td->io_ops_data;

	assert(fio_thread->iocq_count < fio_thread->iocq_size);
	if (status != 0) {
		fio_req->io->error = spdk_fio_status_to_errno(status);
	} else if ((fio_req->io->ddir == DDIR_READ || fio_req->io->ddir == DDIR_WRITE) &&
		   data_size != fio_req->io->xfer_buflen) {
		fio_req->io->error = EIO;
	} else {
		fio_req->io->error = 0;
	}
	fio_thread->iocq[fio_thread->iocq_count++] = fio_req->io;
}

static void
spdk_fio_read_complete(void *cb_arg, struct spdk_io_channel *ch, int status, uint32_t data_size)
{
	spdk_fio_complete_io(cb_arg, status, data_size);
}

static void
spdk_fio_write_complete(void *cb_arg, struct spdk_io_channel *ch, int status, uint32_t data_size)
{
	spdk_fio_complete_io(cb_arg, status, data_size);
}

static void
spdk_fio_flush_complete(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	spdk_fio_complete_io(cb_arg, status, 0);
}

#if FIO_IOOPS_VERSION >= 24
typedef enum fio_q_status fio_q_status_t;
#else
typedef int fio_q_status_t;
#endif

static fio_q_status_t
spdk_fio_queue(struct thread_data *td, struct io_u *io_u)
{
	struct spdk_fio_request *fio_req = io_u->engine_data;
	struct spdk_fio_target *target = io_u->file->engine_data;
	int rc;

	assert(fio_req->td == td);

	if (!target) {
		SPDK_ERRLOG("Unable to look up correct I/O target.\n");
		fio_req->io->error = ENODEV;
		return FIO_Q_COMPLETED;
	}

	fio_req->iov.iov_base = io_u->buf;
	fio_req->iov.iov_len = io_u->xfer_buflen;

	switch (io_u->ddir) {
	case DDIR_READ:
		rc = spdk_fsdev_read(target->desc, target->ch, target->unique++, target->file,
				     target->fhandle, io_u->xfer_buflen, io_u->offset, 0,
				     &fio_req->iov, 1, &fio_req->opts,
				     spdk_fio_read_complete, fio_req);
		break;
	case DDIR_WRITE:
		rc = spdk_fsdev_write(target->desc, target->ch, target->unique++, target->file,
				      target->fhandle, io_u->xfer_buflen, io_u->offset, 0,
				      &fio_req->iov, 1, &fio_req->opts,
				      spdk_fio_write_complete, fio_req);
		break;
	case DDIR_SYNC:
		rc = spdk_fsdev_flush(target->desc, target->ch, target->unique++, target->file,
				      target->fhandle, spdk_fio_flush_complete, fio_req);
		break;
	case DDIR_TRIM:
		rc = -EOPNOTSUPP;
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
	}

	if (rc == -ENOMEM || rc == -ENOBUFS) {
		return FIO_Q_BUSY;
	}

	if (rc != 0) {
		fio_req->io->error = spdk_fio_status_to_errno(rc);
		return FIO_Q_COMPLETED;
	}

	return FIO_Q_QUEUED;
}

static struct io_u *
spdk_fio_event(struct thread_data *td, int event)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;

	assert(event >= 0);
	assert((unsigned)event < fio_thread->iocq_count);
	return fio_thread->iocq[event];
}

static size_t
spdk_fio_poll_thread(struct spdk_fio_thread *fio_thread)
{
	return spdk_thread_poll(fio_thread->thread, 0, 0);
}

static int
spdk_fio_getevents(struct thread_data *td, unsigned int min,
		   unsigned int max, const struct timespec *t)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;
	struct timespec t0, t1;
	uint64_t timeout = 0;

	if (t) {
		timeout = t->tv_sec * SPDK_SEC_TO_NSEC + t->tv_nsec;
		clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
	}

	fio_thread->iocq_count = 0;

	for (;;) {
		spdk_fio_poll_thread(fio_thread);

		if (fio_thread->iocq_count >= min) {
			return fio_thread->iocq_count;
		}

		if (t) {
			clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
			if ((((t1.tv_sec - t0.tv_sec) * SPDK_SEC_TO_NSEC) + t1.tv_nsec - t0.tv_nsec) >
			    timeout) {
				break;
			}
		}
	}

	return fio_thread->iocq_count;
}

static int
spdk_fio_invalidate(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

static struct fio_option options[] = {
	{
		.name		= "spdk_conf",
		.lname		= "SPDK configuration file",
		.type		= FIO_OPT_STR_STORE,
		.off1		= offsetof(struct spdk_fio_options, conf),
		.help		= "A SPDK JSON configuration file",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "spdk_json_conf",
		.lname		= "SPDK JSON configuration file",
		.type		= FIO_OPT_STR_STORE,
		.off1		= offsetof(struct spdk_fio_options, json_conf),
		.help		= "A SPDK JSON configuration file",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "spdk_mem",
		.lname		= "SPDK memory in MB",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, mem_mb),
		.help		= "Amount of memory in MB to allocate for SPDK",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "spdk_single_seg",
		.lname		= "SPDK switch to create just a single hugetlbfs file",
		.type		= FIO_OPT_BOOL,
		.off1		= offsetof(struct spdk_fio_options, mem_single_seg),
		.help		= "If set to 1, SPDK will use just a single hugetlbfs file",
		.def		= "0",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "log_flags",
		.lname		= "log flags",
		.type		= FIO_OPT_STR_STORE,
		.off1		= offsetof(struct spdk_fio_options, log_flags),
		.help		= "SPDK log flags to enable",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "env_context",
		.lname		= "Environment context options",
		.type		= FIO_OPT_STR_STORE,
		.off1		= offsetof(struct spdk_fio_options, env_context),
		.help		= "Opaque context for use of the env implementation",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "spdk_rpc_listen_addr",
		.lname		= "SPDK RPC listen address",
		.type		= FIO_OPT_STR_STORE,
		.off1		= offsetof(struct spdk_fio_options, rpc_listen_addr),
		.help		= "The address to listen the RPC operations",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "fsdev_name",
		.lname		= "SPDK fsdev name",
		.type		= FIO_OPT_STR_STORE,
		.off1		= offsetof(struct spdk_fio_options, fsdev_name),
		.help		= "Name of the SPDK fsdev instance to use",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "fsdev_mount_max_write",
		.lname		= "fsdev mount max write",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, fsdev_mount_max_write),
		.help		= "Maximum write size to advertise during fsdev mount",
		.def		= "0",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "fsdev_writeback_cache",
		.lname		= "fsdev writeback cache",
		.type		= FIO_OPT_BOOL,
		.off1		= offsetof(struct spdk_fio_options, fsdev_writeback_cache),
		.help		= "Enable writeback cache in fsdev mount options",
		.def		= "0",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= "fsdev_create_mode",
		.lname		= "fsdev create mode",
		.type		= FIO_OPT_INT,
		.off1		= offsetof(struct spdk_fio_options, fsdev_create_mode),
		.help		= "Mode used when creating missing files",
		.def		= "0",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_INVALID,
	},
	{
		.name		= NULL,
	},
};

struct ioengine_ops ioengine = {
	.name			= "spdk_fsdev",
	.version		= FIO_IOOPS_VERSION,
	.flags			= FIO_RAWIO | FIO_NOEXTEND | FIO_NODISKUTIL | FIO_MEMALIGN | FIO_DISKLESSIO,
	.setup			= spdk_fio_setup,
	.init			= spdk_fio_init,
	.queue			= spdk_fio_queue,
	.getevents		= spdk_fio_getevents,
	.event			= spdk_fio_event,
	.cleanup		= spdk_fio_cleanup,
	.open_file		= spdk_fio_open,
	.close_file		= spdk_fio_close,
	.invalidate		= spdk_fio_invalidate,
	.iomem_alloc		= spdk_fio_iomem_alloc,
	.iomem_free		= spdk_fio_iomem_free,
	.io_u_init		= spdk_fio_io_u_init,
	.io_u_free		= spdk_fio_io_u_free,
	.option_struct_size	= sizeof(struct spdk_fio_options),
	.options		= options,
};

static void fio_init
spdk_fio_register(void)
{
	register_ioengine(&ioengine);
}

static void
spdk_fio_finish_env(void)
{
	pthread_mutex_lock(&g_init_mtx);
	g_poll_loop = false;
	pthread_cond_signal(&g_init_cond);
	pthread_mutex_unlock(&g_init_mtx);
	pthread_join(g_init_thread_id, NULL);

	spdk_thread_lib_fini();
	spdk_env_fini();
}

static void fio_exit
spdk_fio_unregister(void)
{
	if (g_spdk_env_initialized) {
		spdk_fio_finish_env();
		g_spdk_env_initialized = false;
	}
	unregister_ioengine(&ioengine);
}

SPDK_LOG_REGISTER_COMPONENT(fio_fsdev)
