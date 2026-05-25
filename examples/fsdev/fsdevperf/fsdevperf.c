/*   SPDX-License-Identifier: BSD-3-Clause */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/fsdev.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"

#include "fsdev_fsssd.h"
#include "fsssd_proto.h"

#define FSDEVPERF_DEFAULT_FSDEV_NAME "fsssd0"
#define FSDEVPERF_DEFAULT_FILE_NAME "fsdevperf.dat"
#define FSDEVPERF_DEFAULT_NSID 1
#define FSDEVPERF_DEFAULT_IO_SIZE FSSSD_NFS_PAGE_SIZE
#define FSDEVPERF_DEFAULT_QUEUE_DEPTH 32
#define FSDEVPERF_DEFAULT_TIME_SEC 10
#define FSDEVPERF_DEFAULT_LENGTH (1024ULL * 1024ULL * 1024ULL)
#define FSDEVPERF_UNIQUE_BASE 1

enum fsdevperf_workload {
	FSDEVPERF_READ,
	FSDEVPERF_WRITE,
	FSDEVPERF_RANDREAD,
	FSDEVPERF_RANDWRITE,
};

struct fsdevperf_task {
	struct iovec iov;
	void *buf;
	uint64_t offset;
	uint64_t submit_tsc;
	struct fsdevperf_context *ctx;
};

struct fsdevperf_context {
	const char *fsdev_name;
	const char *device;
	const char *file_name;
	uint32_t nsid;
	uint32_t max_write;
	uint32_t io_size;
	uint32_t queue_depth;
	uint32_t time_in_sec;
	uint64_t offset;
	uint64_t length;
	uint64_t unique;
	uint64_t seed;
	enum fsdevperf_workload workload;
	bool is_draining;
	bool writeback_cache_enabled;

	struct spdk_fsdev *fsdev;
	struct spdk_fsdev_desc *desc;
	struct spdk_io_channel *ch;
	struct spdk_fsdev_file_object *root;
	struct spdk_fsdev_file_object *file;
	struct spdk_fsdev_file_handle *fhandle;
	struct fsdevperf_task *tasks;
	struct spdk_poller *stop_poller;
	struct spdk_poller *stats_poller;

	uint64_t next_offset;
	uint64_t start_tsc;
	uint64_t last_stats_tsc;
	uint64_t completed;
	uint64_t failed;
	uint64_t bytes_completed;
	uint64_t prev_completed;
	uint64_t prev_bytes_completed;
	uint64_t latency_total_tsc;
	uint64_t latency_min_tsc;
	uint64_t latency_max_tsc;
	uint32_t current_queue_depth;
	int rc;
};

static struct fsdevperf_context g_ctx = {
	.fsdev_name = FSDEVPERF_DEFAULT_FSDEV_NAME,
	.file_name = FSDEVPERF_DEFAULT_FILE_NAME,
	.nsid = FSDEVPERF_DEFAULT_NSID,
	.io_size = FSDEVPERF_DEFAULT_IO_SIZE,
	.queue_depth = FSDEVPERF_DEFAULT_QUEUE_DEPTH,
	.time_in_sec = FSDEVPERF_DEFAULT_TIME_SEC,
	.length = FSDEVPERF_DEFAULT_LENGTH,
	.unique = FSDEVPERF_UNIQUE_BASE,
	.workload = FSDEVPERF_WRITE,
};

static const char *
fsdevperf_workload_name(enum fsdevperf_workload workload)
{
	switch (workload) {
	case FSDEVPERF_READ:
		return "read";
	case FSDEVPERF_WRITE:
		return "write";
	case FSDEVPERF_RANDREAD:
		return "randread";
	case FSDEVPERF_RANDWRITE:
		return "randwrite";
	default:
		return "unknown";
	}
}

static bool
fsdevperf_is_read(enum fsdevperf_workload workload)
{
	return workload == FSDEVPERF_READ || workload == FSDEVPERF_RANDREAD;
}

static bool
fsdevperf_is_random(enum fsdevperf_workload workload)
{
	return workload == FSDEVPERF_RANDREAD || workload == FSDEVPERF_RANDWRITE;
}

static void
fsdevperf_usage(void)
{
	printf(" -f <name>       fsdev name (default: %s)\n", FSDEVPERF_DEFAULT_FSDEV_NAME);
	printf(" -F <device>     SPDK NVMe transport id or PCIe BDF for FS-SSD device\n");
	printf(" -N <nsid>       NVMe namespace id (default: %u)\n", FSDEVPERF_DEFAULT_NSID);
	printf(" -P <name>       file name under the root directory (default: %s)\n",
	       FSDEVPERF_DEFAULT_FILE_NAME);
	printf(" -o <workload>   read, write, randread, randwrite (default: write)\n");
	printf(" -q <depth>      queue depth (default: %u)\n", FSDEVPERF_DEFAULT_QUEUE_DEPTH);
	printf(" -t <seconds>    runtime in seconds (default: %u)\n", FSDEVPERF_DEFAULT_TIME_SEC);
	printf(" -S <bytes>      I/O size (default: %u)\n", FSDEVPERF_DEFAULT_IO_SIZE);
	printf(" -z <bytes>      test range length (default: %" PRIu64 ")\n",
	       (uint64_t)FSDEVPERF_DEFAULT_LENGTH);
	printf(" -O <offset>     test range start offset (default: 0)\n");
	printf(" -w <bytes>      max write size to advertise during mount\n");
	printf(" -x              enable writeback cache in mount opts\n");
}

static int
fsdevperf_parse_workload(const char *arg, enum fsdevperf_workload *workload)
{
	if (strcmp(arg, "read") == 0) {
		*workload = FSDEVPERF_READ;
	} else if (strcmp(arg, "write") == 0) {
		*workload = FSDEVPERF_WRITE;
	} else if (strcmp(arg, "randread") == 0) {
		*workload = FSDEVPERF_RANDREAD;
	} else if (strcmp(arg, "randwrite") == 0) {
		*workload = FSDEVPERF_RANDWRITE;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int
fsdevperf_parse_arg(int ch, char *arg)
{
	long value;
	long long value64;

	switch (ch) {
	case 'f':
		g_ctx.fsdev_name = arg;
		break;
	case 'F':
		g_ctx.device = arg;
		break;
	case 'N':
		value = spdk_strtol(arg, 10);
		if (value <= 0 || value > UINT32_MAX) {
			return -EINVAL;
		}
		g_ctx.nsid = value;
		break;
	case 'P':
		g_ctx.file_name = arg;
		break;
	case 'o':
		return fsdevperf_parse_workload(arg, &g_ctx.workload);
	case 'q':
		value = spdk_strtol(arg, 10);
		if (value <= 0 || value > UINT32_MAX) {
			return -EINVAL;
		}
		g_ctx.queue_depth = value;
		break;
	case 't':
		value = spdk_strtol(arg, 10);
		if (value <= 0 || value > UINT32_MAX) {
			return -EINVAL;
		}
		g_ctx.time_in_sec = value;
		break;
	case 'S':
		value = spdk_strtol(arg, 10);
		if (value <= 0 || value > UINT32_MAX) {
			return -EINVAL;
		}
		g_ctx.io_size = value;
		break;
	case 'z':
		value64 = spdk_strtoll(arg, 10);
		if (value64 <= 0) {
			return -EINVAL;
		}
		g_ctx.length = value64;
		break;
	case 'O':
		value64 = spdk_strtoll(arg, 10);
		if (value64 < 0) {
			return -EINVAL;
		}
		g_ctx.offset = value64;
		break;
	case 'w':
		value = spdk_strtol(arg, 10);
		if (value <= 0 || value > UINT32_MAX) {
			return -EINVAL;
		}
		g_ctx.max_write = value;
		break;
	case 'x':
		g_ctx.writeback_cache_enabled = true;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void fsdevperf_cleanup(struct fsdevperf_context *ctx);

static void
fsdevperf_event_cb(enum spdk_fsdev_event_type type, struct spdk_fsdev *fsdev, void *event_ctx)
{
	SPDK_NOTICELOG("fsdev event type %d\n", type);
}

static void
fsdevperf_delete_complete(void *cb_arg, int fsdeverrno)
{
	struct fsdevperf_context *ctx = cb_arg;

	if (fsdeverrno != 0 && ctx->rc == 0) {
		ctx->rc = fsdeverrno;
	}
	spdk_app_stop(ctx->rc);
}

static void
fsdevperf_free_tasks(struct fsdevperf_context *ctx)
{
	uint32_t i;

	if (ctx->tasks == NULL) {
		return;
	}

	for (i = 0; i < ctx->queue_depth; i++) {
		spdk_dma_free(ctx->tasks[i].buf);
	}
	free(ctx->tasks);
	ctx->tasks = NULL;
}

static void
fsdevperf_cleanup(struct fsdevperf_context *ctx)
{
	spdk_poller_unregister(&ctx->stop_poller);
	spdk_poller_unregister(&ctx->stats_poller);

	if (ctx->ch != NULL) {
		spdk_put_io_channel(ctx->ch);
		ctx->ch = NULL;
	}
	if (ctx->desc != NULL) {
		spdk_fsdev_close(ctx->desc);
		ctx->desc = NULL;
	}

	fsdevperf_free_tasks(ctx);

	if (ctx->fsdev != NULL) {
		spdk_fsdev_fsssd_delete(ctx->fsdev_name, fsdevperf_delete_complete, ctx);
		ctx->fsdev = NULL;
		return;
	}

	spdk_app_stop(ctx->rc);
}

static void
fsdevperf_umount_complete(void *cb_arg, struct spdk_io_channel *ch)
{
	struct fsdevperf_context *ctx = cb_arg;

	ctx->root = NULL;
	fsdevperf_cleanup(ctx);
}

static void
fsdevperf_umount(struct fsdevperf_context *ctx)
{
	int rc;

	if (ctx->root == NULL) {
		fsdevperf_cleanup(ctx);
		return;
	}

	rc = spdk_fsdev_umount(ctx->desc, ctx->ch, ctx->unique++, fsdevperf_umount_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_umount failed: %d\n", rc);
		if (ctx->rc == 0) {
			ctx->rc = rc;
		}
		fsdevperf_cleanup(ctx);
	}
}

static void
fsdevperf_forget_complete(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fsdevperf_context *ctx = cb_arg;

	if (status != 0 && ctx->rc == 0) {
		ctx->rc = status;
	}
	ctx->file = NULL;
	fsdevperf_umount(ctx);
}

static void
fsdevperf_forget_file(struct fsdevperf_context *ctx)
{
	int rc;

	if (ctx->file == NULL) {
		fsdevperf_umount(ctx);
		return;
	}

	rc = spdk_fsdev_forget(ctx->desc, ctx->ch, ctx->unique++, ctx->file, 1,
			       fsdevperf_forget_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_forget failed: %d\n", rc);
		if (ctx->rc == 0) {
			ctx->rc = rc;
		}
		fsdevperf_umount(ctx);
	}
}

static void
fsdevperf_release_complete(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fsdevperf_context *ctx = cb_arg;

	if (status != 0 && ctx->rc == 0) {
		ctx->rc = status;
	}
	ctx->fhandle = NULL;
	fsdevperf_forget_file(ctx);
}

static void
fsdevperf_release_file(struct fsdevperf_context *ctx)
{
	int rc;

	if (ctx->fhandle == NULL) {
		fsdevperf_forget_file(ctx);
		return;
	}

	rc = spdk_fsdev_release(ctx->desc, ctx->ch, ctx->unique++, ctx->file, ctx->fhandle,
				fsdevperf_release_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_release failed: %d\n", rc);
		if (ctx->rc == 0) {
			ctx->rc = rc;
		}
		fsdevperf_forget_file(ctx);
	}
}

static void
fsdevperf_dump_final_stats(struct fsdevperf_context *ctx)
{
	uint64_t now = spdk_get_ticks();
	uint64_t elapsed_tsc = now - ctx->start_tsc;
	double elapsed_sec = (double)elapsed_tsc / spdk_get_ticks_hz();
	double iops = elapsed_sec > 0 ? (double)ctx->completed / elapsed_sec : 0.0;
	double mib_s = elapsed_sec > 0 ? (double)ctx->bytes_completed / (1024.0 * 1024.0) / elapsed_sec : 0.0;
	double avg_us = 0.0;
	double min_us = 0.0;
	double max_us = 0.0;

	if (ctx->completed != 0) {
		avg_us = (double)ctx->latency_total_tsc / ctx->completed * SPDK_SEC_TO_USEC /
			 spdk_get_ticks_hz();
		min_us = (double)ctx->latency_min_tsc * SPDK_SEC_TO_USEC / spdk_get_ticks_hz();
		max_us = (double)ctx->latency_max_tsc * SPDK_SEC_TO_USEC / spdk_get_ticks_hz();
	}

	printf("\n");
	printf("fsdevperf result: workload=%s runtime=%.2f sec completed=%" PRIu64
	       " failed=%" PRIu64 "\n",
	       fsdevperf_workload_name(ctx->workload), elapsed_sec, ctx->completed, ctx->failed);
	printf("  IOPS %.2f, MiB/s %.2f, latency usec avg %.2f min %.2f max %.2f\n",
	       iops, mib_s, avg_us, min_us, max_us);
}

static void
fsdevperf_finish_run(struct fsdevperf_context *ctx)
{
	spdk_poller_unregister(&ctx->stop_poller);
	spdk_poller_unregister(&ctx->stats_poller);
	fsdevperf_dump_final_stats(ctx);
	fsdevperf_release_file(ctx);
}

static uint64_t
fsdevperf_next_offset(struct fsdevperf_context *ctx)
{
	uint64_t slots = ctx->length / ctx->io_size;
	uint64_t slot;

	if (fsdevperf_is_random(ctx->workload)) {
		slot = spdk_rand_xorshift64(&ctx->seed) % slots;
		return ctx->offset + slot * ctx->io_size;
	}

	slot = (ctx->next_offset - ctx->offset) / ctx->io_size;
	ctx->next_offset += ctx->io_size;
	if (++slot == slots) {
		ctx->next_offset = ctx->offset;
	}

	return ctx->offset + (slot - 1) * ctx->io_size;
}

static void fsdevperf_submit_task(struct fsdevperf_task *task);

static void
fsdevperf_io_complete(void *cb_arg, struct spdk_io_channel *ch, int status, uint32_t data_size)
{
	struct fsdevperf_task *task = cb_arg;
	struct fsdevperf_context *ctx = task->ctx;
	uint64_t latency_tsc = spdk_get_ticks() - task->submit_tsc;

	assert(ctx->current_queue_depth > 0);
	ctx->current_queue_depth--;

	if (status == 0 && data_size == ctx->io_size) {
		ctx->completed++;
		ctx->bytes_completed += data_size;
		ctx->latency_total_tsc += latency_tsc;
		if (ctx->latency_min_tsc == 0 || latency_tsc < ctx->latency_min_tsc) {
			ctx->latency_min_tsc = latency_tsc;
		}
		if (latency_tsc > ctx->latency_max_tsc) {
			ctx->latency_max_tsc = latency_tsc;
		}
	} else {
		ctx->failed++;
		if (ctx->rc == 0) {
			ctx->rc = status != 0 ? status : -EIO;
		}
		ctx->is_draining = true;
		SPDK_ERRLOG("I/O failed: status=%d data_size=%u offset=%" PRIu64 "\n",
			    status, data_size, task->offset);
	}

	if (!ctx->is_draining) {
		fsdevperf_submit_task(task);
	} else if (ctx->current_queue_depth == 0) {
		fsdevperf_finish_run(ctx);
	}
}

static void
fsdevperf_submit_task(struct fsdevperf_task *task)
{
	struct fsdevperf_context *ctx = task->ctx;
	int rc;

	task->offset = fsdevperf_next_offset(ctx);
	task->submit_tsc = spdk_get_ticks();
	task->iov.iov_base = task->buf;
	task->iov.iov_len = ctx->io_size;

	if (fsdevperf_is_read(ctx->workload)) {
		rc = spdk_fsdev_read(ctx->desc, ctx->ch, ctx->unique++, ctx->file, ctx->fhandle,
				     ctx->io_size, task->offset, 0, &task->iov, 1, NULL,
				     fsdevperf_io_complete, task);
	} else {
		rc = spdk_fsdev_write(ctx->desc, ctx->ch, ctx->unique++, ctx->file, ctx->fhandle,
				      ctx->io_size, task->offset, 0, &task->iov, 1, NULL,
				      fsdevperf_io_complete, task);
	}

	if (rc != 0) {
		SPDK_ERRLOG("failed to submit I/O: %d\n", rc);
		if (ctx->rc == 0) {
			ctx->rc = rc;
		}
		ctx->is_draining = true;
		if (ctx->current_queue_depth == 0) {
			fsdevperf_finish_run(ctx);
		}
		return;
	}

	ctx->current_queue_depth++;
}

static int
fsdevperf_stop_poller(void *arg)
{
	struct fsdevperf_context *ctx = arg;

	ctx->is_draining = true;
	spdk_poller_unregister(&ctx->stop_poller);
	if (ctx->current_queue_depth == 0) {
		fsdevperf_finish_run(ctx);
	}

	return SPDK_POLLER_IDLE;
}

static int
fsdevperf_stats_poller(void *arg)
{
	struct fsdevperf_context *ctx = arg;
	uint64_t now = spdk_get_ticks();
	uint64_t period_tsc = now - ctx->last_stats_tsc;
	uint64_t done = ctx->completed - ctx->prev_completed;
	uint64_t bytes = ctx->bytes_completed - ctx->prev_bytes_completed;
	double period_sec = (double)period_tsc / spdk_get_ticks_hz();
	double iops = period_sec > 0 ? (double)done / period_sec : 0.0;
	double mib_s = period_sec > 0 ? (double)bytes / (1024.0 * 1024.0) / period_sec : 0.0;

	ctx->last_stats_tsc = now;
	ctx->prev_completed = ctx->completed;
	ctx->prev_bytes_completed = ctx->bytes_completed;

	printf("%12.2f IOPS, %8.2f MiB/s, failed=%" PRIu64 "\r", iops, mib_s, ctx->failed);
	fflush(stdout);

	return SPDK_POLLER_IDLE;
}

static int
fsdevperf_alloc_tasks(struct fsdevperf_context *ctx)
{
	uint32_t i;
	uint8_t *buf;
	uint32_t j;

	ctx->tasks = calloc(ctx->queue_depth, sizeof(*ctx->tasks));
	if (ctx->tasks == NULL) {
		return -ENOMEM;
	}

	for (i = 0; i < ctx->queue_depth; i++) {
		buf = spdk_dma_zmalloc(ctx->io_size, FSSSD_NFS_PAGE_SIZE, NULL);
		if (buf == NULL) {
			return -ENOMEM;
		}

		for (j = 0; j < ctx->io_size; j++) {
			buf[j] = (uint8_t)(j + i);
		}
		ctx->tasks[i].ctx = ctx;
		ctx->tasks[i].buf = buf;
	}

	return 0;
}

static void
fsdevperf_start_io(struct fsdevperf_context *ctx)
{
	uint32_t i;
	int rc;

	rc = fsdevperf_alloc_tasks(ctx);
	if (rc != 0) {
		SPDK_ERRLOG("failed to allocate tasks: %d\n", rc);
		ctx->rc = rc;
		fsdevperf_release_file(ctx);
		return;
	}

	ctx->next_offset = ctx->offset;
	ctx->seed = spdk_rand_xorshift64_seed();
	ctx->start_tsc = spdk_get_ticks();
	ctx->last_stats_tsc = ctx->start_tsc;

	printf("Running fsdevperf: file=%s workload=%s qd=%u io_size=%u range=[%" PRIu64
	       ", %" PRIu64 ") time=%u sec\n",
	       ctx->file_name, fsdevperf_workload_name(ctx->workload), ctx->queue_depth,
	       ctx->io_size, ctx->offset, ctx->offset + ctx->length, ctx->time_in_sec);
	fflush(stdout);

	ctx->stop_poller = SPDK_POLLER_REGISTER(fsdevperf_stop_poller, ctx,
						ctx->time_in_sec * SPDK_SEC_TO_USEC);
	ctx->stats_poller = SPDK_POLLER_REGISTER(fsdevperf_stats_poller, ctx, SPDK_SEC_TO_USEC);

	for (i = 0; i < ctx->queue_depth; i++) {
		fsdevperf_submit_task(&ctx->tasks[i]);
		if (ctx->is_draining) {
			break;
		}
	}
}

static void
fsdevperf_open_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
			struct spdk_fsdev_file_handle *fhandle)
{
	struct fsdevperf_context *ctx = cb_arg;

	if (status != 0) {
		SPDK_ERRLOG("open failed: %d\n", status);
		ctx->rc = status;
		fsdevperf_forget_file(ctx);
		return;
	}

	ctx->fhandle = fhandle;
	fsdevperf_start_io(ctx);
}

static void
fsdevperf_open_file(struct fsdevperf_context *ctx)
{
	int rc;

	rc = spdk_fsdev_fopen(ctx->desc, ctx->ch, ctx->unique++, ctx->file, O_RDWR,
			      fsdevperf_open_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_fopen failed: %d\n", rc);
		ctx->rc = rc;
		fsdevperf_forget_file(ctx);
	}
}

static void
fsdevperf_create_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
			  struct spdk_fsdev_file_object *fobject,
			  const struct spdk_fsdev_file_attr *attr,
			  struct spdk_fsdev_file_handle *fhandle)
{
	struct fsdevperf_context *ctx = cb_arg;

	if (status != 0) {
		SPDK_ERRLOG("create failed: %d\n", status);
		ctx->rc = status;
		fsdevperf_umount(ctx);
		return;
	}

	ctx->file = fobject;
	ctx->fhandle = fhandle;
	fsdevperf_start_io(ctx);
}

static void
fsdevperf_create_file(struct fsdevperf_context *ctx)
{
	int rc;

	rc = spdk_fsdev_create(ctx->desc, ctx->ch, ctx->unique++, ctx->root, ctx->file_name,
			       S_IFREG | 0644, O_RDWR, 0, 0, 0,
			       fsdevperf_create_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_create failed: %d\n", rc);
		ctx->rc = rc;
		fsdevperf_umount(ctx);
	}
}

static void
fsdevperf_lookup_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
			  struct spdk_fsdev_file_object *fobject,
			  const struct spdk_fsdev_file_attr *attr)
{
	struct fsdevperf_context *ctx = cb_arg;

	if (status == -ENOENT) {
		fsdevperf_create_file(ctx);
		return;
	}
	if (status != 0) {
		SPDK_ERRLOG("lookup failed: %d\n", status);
		ctx->rc = status;
		fsdevperf_umount(ctx);
		return;
	}

	ctx->file = fobject;
	fsdevperf_open_file(ctx);
}

static void
fsdevperf_mount_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
			 const struct spdk_fsdev_mount_opts *opts,
			 struct spdk_fsdev_file_object *root_fobject)
{
	struct fsdevperf_context *ctx = cb_arg;
	int rc;

	if (status != 0) {
		SPDK_ERRLOG("mount failed: %d\n", status);
		ctx->rc = status;
		fsdevperf_cleanup(ctx);
		return;
	}

	ctx->root = root_fobject;
	rc = spdk_fsdev_lookup(ctx->desc, ctx->ch, ctx->unique++, ctx->root, ctx->file_name,
			       fsdevperf_lookup_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_lookup failed: %d\n", rc);
		ctx->rc = rc;
		fsdevperf_umount(ctx);
	}
}

static int
fsdevperf_validate(struct fsdevperf_context *ctx)
{
	if (ctx->device == NULL) {
		SPDK_ERRLOG("missing required -F <device> argument\n");
		return -EINVAL;
	}
	if (ctx->io_size % FSSSD_NFS_PAGE_SIZE != 0 || ctx->offset % FSSSD_NFS_PAGE_SIZE != 0 ||
	    ctx->length % FSSSD_NFS_PAGE_SIZE != 0) {
		SPDK_ERRLOG("I/O size, offset, and length must be %u-byte aligned\n",
			    FSSSD_NFS_PAGE_SIZE);
		return -EINVAL;
	}
	if (ctx->length < ctx->io_size) {
		SPDK_ERRLOG("length must be at least I/O size\n");
		return -EINVAL;
	}

	return 0;
}

static void
fsdevperf_start(void *arg1)
{
	struct fsdevperf_context *ctx = arg1;
	struct spdk_fsdev_fsssd_opts fsssd_opts = {};
	struct spdk_fsdev_mount_opts mount_opts = {};
	int rc;

	rc = fsdevperf_validate(ctx);
	if (rc != 0) {
		spdk_app_stop(rc);
		return;
	}

	spdk_fsdev_fsssd_get_default_opts(&fsssd_opts);
	fsssd_opts.device = (char *)ctx->device;
	fsssd_opts.nsid = ctx->nsid;
	fsssd_opts.max_write = ctx->max_write;
	fsssd_opts.writeback_cache_enabled = ctx->writeback_cache_enabled;

	rc = spdk_fsdev_fsssd_create(&ctx->fsdev, ctx->fsdev_name, &fsssd_opts);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_fsssd_create failed: %d\n", rc);
		spdk_app_stop(rc);
		return;
	}

	rc = spdk_fsdev_open(ctx->fsdev_name, fsdevperf_event_cb, NULL, &ctx->desc);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_open failed: %d\n", rc);
		ctx->rc = rc;
		fsdevperf_cleanup(ctx);
		return;
	}

	ctx->ch = spdk_fsdev_get_io_channel(ctx->desc);
	if (ctx->ch == NULL) {
		SPDK_ERRLOG("spdk_fsdev_get_io_channel failed\n");
		ctx->rc = -ENOMEM;
		fsdevperf_cleanup(ctx);
		return;
	}

	mount_opts.opts_size = sizeof(mount_opts);
	mount_opts.max_write = ctx->max_write ? ctx->max_write : UINT32_MAX;
	mount_opts.writeback_cache_enabled = ctx->writeback_cache_enabled;

	rc = spdk_fsdev_mount(ctx->desc, ctx->ch, ctx->unique++, &mount_opts,
			      fsdevperf_mount_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_mount failed: %d\n", rc);
		ctx->rc = rc;
		fsdevperf_cleanup(ctx);
	}
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "fsdevperf";

	rc = spdk_app_parse_args(argc, argv, &opts, "f:F:N:P:o:q:t:S:z:O:w:x", NULL,
				 fsdevperf_parse_arg, fsdevperf_usage);
	if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	rc = spdk_app_start(&opts, fsdevperf_start, &g_ctx);
	spdk_app_fini();

	return rc;
}
