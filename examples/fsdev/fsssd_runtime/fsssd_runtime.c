/*   SPDX-License-Identifier: BSD-3-Clause */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/fsdev.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include "fsdev_fsssd.h"

#define FSSSD_RUNTIME_DEFAULT_FSDEV_NAME "fsssd0"
#define FSSSD_RUNTIME_DEFAULT_NSID 1
#define FSSSD_RUNTIME_UNIQUE_BASE 1

struct fsssd_runtime_ctx {
	const char *fsdev_name;
	const char *device;
	uint32_t nsid;
	uint32_t max_write;
	bool writeback_cache_enabled;
	uint64_t unique;
	struct spdk_fsdev *fsdev;
	struct spdk_fsdev_desc *desc;
	struct spdk_io_channel *ch;
	struct spdk_fsdev_file_object *root;
	int rc;
};

static struct fsssd_runtime_ctx g_ctx = {
	.fsdev_name = FSSSD_RUNTIME_DEFAULT_FSDEV_NAME,
	.nsid = FSSSD_RUNTIME_DEFAULT_NSID,
	.unique = FSSSD_RUNTIME_UNIQUE_BASE,
};

static void
fsssd_runtime_usage(void)
{
	printf(" -f <name>       fsdev name (default: %s)\n", FSSSD_RUNTIME_DEFAULT_FSDEV_NAME);
	printf(" -F <device>     SPDK NVMe transport id or PCIe BDF for FS-SSD device\n");
	printf(" -N <nsid>       NVMe namespace id (default: %u)\n", FSSSD_RUNTIME_DEFAULT_NSID);
	printf(" -w <bytes>      max write size to advertise during mount\n");
	printf(" -x              enable writeback cache in mount opts (Currently, fsssd backend does not support writeback)\n");
}

static int
fsssd_runtime_parse_arg(int ch, char *arg)
{
	long value;

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

static void
fsssd_runtime_event_cb(enum spdk_fsdev_event_type type, struct spdk_fsdev *fsdev, void *event_ctx)
{
	SPDK_NOTICELOG("fsdev event type %d\n", type);
}

static void
fsssd_runtime_delete_complete(void *cb_arg, int fsdeverrno)
{
	struct fsssd_runtime_ctx *ctx = cb_arg;

	if (fsdeverrno != 0 && ctx->rc == 0) {
		ctx->rc = fsdeverrno;
	}
	SPDK_NOTICELOG("delete complete status=%d\n", fsdeverrno);
	spdk_app_stop(ctx->rc);
}

static void
fsssd_runtime_cleanup(struct fsssd_runtime_ctx *ctx)
{
	if (ctx->ch != NULL) {
		spdk_put_io_channel(ctx->ch);
		ctx->ch = NULL;
	}
	if (ctx->desc != NULL) {
		spdk_fsdev_close(ctx->desc);
		ctx->desc = NULL;
	}
	if (ctx->fsdev != NULL) {
		spdk_fsdev_fsssd_delete(ctx->fsdev_name, fsssd_runtime_delete_complete, ctx);
		ctx->fsdev = NULL;
		return;
	}

	spdk_app_stop(ctx->rc);
}

static void
fsssd_runtime_umount_complete(void *cb_arg, struct spdk_io_channel *ch)
{
	struct fsssd_runtime_ctx *ctx = cb_arg;

	SPDK_NOTICELOG("umount complete\n");
	ctx->root = NULL;
	fsssd_runtime_cleanup(ctx);
}

static void
fsssd_runtime_umount(struct fsssd_runtime_ctx *ctx)
{
	int rc;

	rc = spdk_fsdev_umount(ctx->desc, ctx->ch, ctx->unique++, fsssd_runtime_umount_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_umount failed: %d\n", rc);
		ctx->rc = rc;
		fsssd_runtime_cleanup(ctx);
	}
}

static void
fsssd_runtime_statfs_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
				     const struct spdk_fsdev_file_statfs *statfs)
{
	struct fsssd_runtime_ctx *ctx = cb_arg;

	if (status != 0) {
		SPDK_ERRLOG("statfs failed: %d\n", status);
		ctx->rc = status;
		fsssd_runtime_umount(ctx);
		return;
	}

	SPDK_NOTICELOG("statfs: blocks=%" PRIu64 " bfree=%" PRIu64 " bavail=%" PRIu64
		       " files=%" PRIu64 " ffree=%" PRIu64 " bsize=%u namelen=%u frsize=%u\n",
		       statfs->blocks, statfs->bfree, statfs->bavail, statfs->files, statfs->ffree,
		       statfs->bsize, statfs->namelen, statfs->frsize);
	fsssd_runtime_umount(ctx);
}

static void
fsssd_runtime_getattr_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
				      const struct spdk_fsdev_file_attr *attr)
{
	struct fsssd_runtime_ctx *ctx = cb_arg;
	int rc;

	if (status != 0) {
		SPDK_ERRLOG("getattr(root) failed: %d\n", status);
		ctx->rc = status;
		fsssd_runtime_umount(ctx);
		return;
	}

	SPDK_NOTICELOG("root attr: ino=%" PRIu64 " mode=0%o size=%" PRIu64 " blocks=%" PRIu64
		       " nlink=%u uid=%u gid=%u blksize=%u\n",
		       attr->ino, attr->mode, attr->size, attr->blocks, attr->nlink, attr->uid, attr->gid,
		       attr->blksize);

	rc = spdk_fsdev_statfs(ctx->desc, ctx->ch, ctx->unique++, ctx->root,
				   fsssd_runtime_statfs_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_statfs failed: %d\n", rc);
		ctx->rc = rc;
		fsssd_runtime_umount(ctx);
	}
}

static void
fsssd_runtime_mount_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
				    const struct spdk_fsdev_mount_opts *opts,
				    struct spdk_fsdev_file_object *root_fobject)
{
	struct fsssd_runtime_ctx *ctx = cb_arg;
	int rc;

	if (status != 0) {
		SPDK_ERRLOG("mount failed: %d\n", status);
		ctx->rc = status;
		fsssd_runtime_cleanup(ctx);
		return;
	}

	ctx->root = root_fobject;
	SPDK_NOTICELOG("mount complete: root=%p max_write=%u writeback=%u\n", root_fobject,
		       opts->max_write, opts->writeback_cache_enabled);

	rc = spdk_fsdev_getattr(ctx->desc, ctx->ch, ctx->unique++, ctx->root, NULL,
				    fsssd_runtime_getattr_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_getattr failed: %d\n", rc);
		ctx->rc = rc;
		fsssd_runtime_umount(ctx);
	}
}

static void
fsssd_runtime_start(void *arg1)
{
	struct fsssd_runtime_ctx *ctx = arg1;
	struct spdk_fsdev_fsssd_opts fsssd_opts = {};
	struct spdk_fsdev_mount_opts mount_opts = {};
	int rc;

	if (ctx->device == NULL) {
		SPDK_ERRLOG("missing required -F <device> argument\n");
		spdk_app_stop(-EINVAL);
		return;
	}

	spdk_fsdev_fsssd_get_default_opts(&fsssd_opts);
	fsssd_opts.device = (char *)ctx->device;
	fsssd_opts.nsid = ctx->nsid;
	fsssd_opts.max_write = ctx->max_write;
	fsssd_opts.writeback_cache_enabled = ctx->writeback_cache_enabled;

	SPDK_NOTICELOG("creating fsdev_fsssd name=%s device=%s nsid=%u\n",
		       ctx->fsdev_name, ctx->device, ctx->nsid);
	rc = spdk_fsdev_fsssd_create(&ctx->fsdev, ctx->fsdev_name, &fsssd_opts);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_fsssd_create failed: %d\n", rc);
		spdk_app_stop(rc);
		return;
	}

	rc = spdk_fsdev_open(ctx->fsdev_name, fsssd_runtime_event_cb, NULL, &ctx->desc);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_open failed: %d\n", rc);
		ctx->rc = rc;
		fsssd_runtime_cleanup(ctx);
		return;
	}

	ctx->ch = spdk_fsdev_get_io_channel(ctx->desc);
	if (ctx->ch == NULL) {
		SPDK_ERRLOG("spdk_fsdev_get_io_channel failed\n");
		ctx->rc = -ENOMEM;
		fsssd_runtime_cleanup(ctx);
		return;
	}

	mount_opts.opts_size = sizeof(mount_opts);
	mount_opts.max_write = ctx->max_write ? ctx->max_write : UINT32_MAX;
	mount_opts.writeback_cache_enabled = ctx->writeback_cache_enabled;

	rc = spdk_fsdev_mount(ctx->desc, ctx->ch, ctx->unique++, &mount_opts,
				  fsssd_runtime_mount_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_mount failed: %d\n", rc);
		ctx->rc = rc;
		fsssd_runtime_cleanup(ctx);
	}
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "fsssd_runtime";

	rc = spdk_app_parse_args(argc, argv, &opts, "f:F:N:w:x", NULL,
				 fsssd_runtime_parse_arg, fsssd_runtime_usage);
	if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	rc = spdk_app_start(&opts, fsssd_runtime_start, &g_ctx);
	spdk_app_fini();

	return rc;
}
