/*   SPDX-License-Identifier: BSD-3-Clause */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/fsdev.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include "fsdev_fsssd.h"
#include "fsssd_proto.h"

#define FSSSD_RUNTIME_DEFAULT_FSDEV_NAME "fsssd0"
#define FSSSD_RUNTIME_DEFAULT_NSID 1
#define FSSSD_RUNTIME_UNIQUE_BASE 1
#define FSSSD_RUNTIME_DEFAULT_IO_SIZE FSSSD_NFS_PAGE_SIZE

struct fsssd_runtime_ctx {
	const char *fsdev_name;
	const char *device;
	const char *file_name;
	uint32_t nsid;
	uint32_t max_write;
	uint32_t io_size;
	uint64_t io_offset;
	bool writeback_cache_enabled;
	uint64_t unique;
	struct spdk_fsdev *fsdev;
	struct spdk_fsdev_desc *desc;
	struct spdk_io_channel *ch;
	struct spdk_fsdev_file_object *root;
	struct spdk_fsdev_file_object *file;
	struct spdk_fsdev_file_handle *fhandle;
	uint8_t *write_buf;
	uint8_t *read_buf;
	struct iovec write_iov[2];
	struct iovec read_iov[2];
	int rc;
};

static struct fsssd_runtime_ctx g_ctx = {
	.fsdev_name = FSSSD_RUNTIME_DEFAULT_FSDEV_NAME,
	.nsid = FSSSD_RUNTIME_DEFAULT_NSID,
	.io_size = FSSSD_RUNTIME_DEFAULT_IO_SIZE,
	.unique = FSSSD_RUNTIME_UNIQUE_BASE,
};

static void
fsssd_runtime_usage(void)
{
	printf(" -f <name>       fsdev name (default: %s)\n", FSSSD_RUNTIME_DEFAULT_FSDEV_NAME);
	printf(" -F <device>     SPDK NVMe transport id or PCIe BDF for FS-SSD device\n");
	printf(" -N <nsid>       NVMe namespace id (default: %u)\n", FSSSD_RUNTIME_DEFAULT_NSID);
	printf(" -P <name>       optional file name to run lookup/open/write/read/compare test\n");
	printf(" -O <offset>     data test offset (default: 0)\n");
	printf(" -S <bytes>      data test size (default: %u)\n", FSSSD_RUNTIME_DEFAULT_IO_SIZE);
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
	case 'P':
		g_ctx.file_name = arg;
		break;
	case 'O':
		value = spdk_strtoll(arg, 10);
		if (value < 0) {
			return -EINVAL;
		}
		g_ctx.io_offset = value;
		break;
	case 'S':
		value = spdk_strtol(arg, 10);
		if (value <= 0 || value > UINT32_MAX) {
			return -EINVAL;
		}
		g_ctx.io_size = value;
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
	if (ctx->write_buf != NULL) {
		spdk_dma_free(ctx->write_buf);
		ctx->write_buf = NULL;
	}
	if (ctx->read_buf != NULL) {
		spdk_dma_free(ctx->read_buf);
		ctx->read_buf = NULL;
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
fsssd_runtime_forget_file_complete(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fsssd_runtime_ctx *ctx = cb_arg;

	SPDK_NOTICELOG("forget complete status=%d\n", status);
	if (status != 0 && ctx->rc == 0) {
		ctx->rc = status;
	}
	ctx->file = NULL;
	fsssd_runtime_umount(ctx);
}

static void
fsssd_runtime_forget_file(struct fsssd_runtime_ctx *ctx)
{
	int rc;

	if (ctx->file == NULL) {
		fsssd_runtime_umount(ctx);
		return;
	}

	rc = spdk_fsdev_forget(ctx->desc, ctx->ch, ctx->unique++, ctx->file, 1,
			       fsssd_runtime_forget_file_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_forget failed: %d\n", rc);
		ctx->rc = rc;
		fsssd_runtime_umount(ctx);
	}
}

static void
fsssd_runtime_release_complete(void *cb_arg, struct spdk_io_channel *ch, int status)
{
	struct fsssd_runtime_ctx *ctx = cb_arg;

	SPDK_NOTICELOG("release complete status=%d\n", status);
	if (status != 0 && ctx->rc == 0) {
		ctx->rc = status;
	}
	ctx->fhandle = NULL;
	fsssd_runtime_forget_file(ctx);
}

static void
fsssd_runtime_release_file(struct fsssd_runtime_ctx *ctx)
{
	int rc;

	if (ctx->fhandle == NULL) {
		fsssd_runtime_forget_file(ctx);
		return;
	}

	rc = spdk_fsdev_release(ctx->desc, ctx->ch, ctx->unique++, ctx->file, ctx->fhandle,
				fsssd_runtime_release_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_release failed: %d\n", rc);
		ctx->rc = rc;
		fsssd_runtime_forget_file(ctx);
	}
}

static int
fsssd_runtime_alloc_io_buffers(struct fsssd_runtime_ctx *ctx)
{
	if (ctx->write_buf != NULL && ctx->read_buf != NULL) {
		return 0;
	}

	ctx->write_buf = spdk_dma_zmalloc(ctx->io_size, FSSSD_NFS_PAGE_SIZE, NULL);
	ctx->read_buf = spdk_dma_zmalloc(ctx->io_size, FSSSD_NFS_PAGE_SIZE, NULL);
	if (ctx->write_buf == NULL || ctx->read_buf == NULL) {
		return -ENOMEM;
	}

	return 0;
}

static void
fsssd_runtime_fill_write_buffer(struct fsssd_runtime_ctx *ctx)
{
	uint32_t i;

	for (i = 0; i < ctx->io_size; i++) {
		ctx->write_buf[i] = (uint8_t)(i ^ 0xa5);
	}
}

static void
fsssd_runtime_setup_iovs(struct iovec *iov, uint8_t *buf, uint32_t size, uint32_t first_len)
{
	iov[0].iov_base = buf;
	iov[0].iov_len = first_len;
	iov[1].iov_base = buf + first_len;
	iov[1].iov_len = size - first_len;
}

static void fsssd_runtime_read_file(struct fsssd_runtime_ctx *ctx);

static void
fsssd_runtime_write_complete(void *cb_arg, struct spdk_io_channel *ch, int status, uint32_t data_size)
{
	struct fsssd_runtime_ctx *ctx = cb_arg;

	SPDK_NOTICELOG("write complete status=%d data_size=%u\n", status, data_size);
	if (status != 0) {
		ctx->rc = status;
		fsssd_runtime_release_file(ctx);
		return;
	}
	if (data_size != ctx->io_size) {
		SPDK_ERRLOG("write size mismatch: %u != %u\n", data_size, ctx->io_size);
		ctx->rc = -EIO;
		fsssd_runtime_release_file(ctx);
		return;
	}

	fsssd_runtime_read_file(ctx);
}

static void
fsssd_runtime_write_file(struct fsssd_runtime_ctx *ctx)
{
	int rc;

	rc = fsssd_runtime_alloc_io_buffers(ctx);
	if (rc != 0) {
		SPDK_ERRLOG("failed to allocate IO buffers: %d\n", rc);
		ctx->rc = rc;
		fsssd_runtime_release_file(ctx);
		return;
	}

	fsssd_runtime_fill_write_buffer(ctx);
	memset(ctx->read_buf, 0, ctx->io_size);
	fsssd_runtime_setup_iovs(ctx->write_iov, ctx->write_buf, ctx->io_size, ctx->io_size / 2);

	SPDK_NOTICELOG("write file=%s offset=%" PRIu64 " size=%u iovcnt=2\n",
		       ctx->file_name, ctx->io_offset, ctx->io_size);
	rc = spdk_fsdev_write(ctx->desc, ctx->ch, ctx->unique++, ctx->file, ctx->fhandle,
			      ctx->io_size, ctx->io_offset, 0, ctx->write_iov, 2, NULL,
			      fsssd_runtime_write_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_write failed: %d\n", rc);
		ctx->rc = rc;
		fsssd_runtime_release_file(ctx);
	}
}

static void
fsssd_runtime_read_complete(void *cb_arg, struct spdk_io_channel *ch, int status, uint32_t data_size)
{
	struct fsssd_runtime_ctx *ctx = cb_arg;
	uint32_t i;

	SPDK_NOTICELOG("read complete status=%d data_size=%u\n", status, data_size);
	if (status != 0) {
		ctx->rc = status;
		fsssd_runtime_release_file(ctx);
		return;
	}
	if (data_size != ctx->io_size) {
		SPDK_ERRLOG("read size mismatch: %u != %u\n", data_size, ctx->io_size);
		ctx->rc = -EIO;
		fsssd_runtime_release_file(ctx);
		return;
	}
	if (memcmp(ctx->write_buf, ctx->read_buf, ctx->io_size) != 0) {
		for (i = 0; i < ctx->io_size; i++) {
			if (ctx->write_buf[i] != ctx->read_buf[i]) {
				SPDK_ERRLOG("data mismatch at offset %u: 0x%02x != 0x%02x\n",
					    i, ctx->read_buf[i], ctx->write_buf[i]);
				break;
			}
		}
		ctx->rc = -EIO;
	} else {
		SPDK_NOTICELOG("data compare passed\n");
	}

	fsssd_runtime_release_file(ctx);
}

static void
fsssd_runtime_read_file(struct fsssd_runtime_ctx *ctx)
{
	int rc;

	memset(ctx->read_buf, 0, ctx->io_size);
	fsssd_runtime_setup_iovs(ctx->read_iov, ctx->read_buf, ctx->io_size, ctx->io_size / 4);

	SPDK_NOTICELOG("read file=%s offset=%" PRIu64 " size=%u iovcnt=2\n",
		       ctx->file_name, ctx->io_offset, ctx->io_size);
	rc = spdk_fsdev_read(ctx->desc, ctx->ch, ctx->unique++, ctx->file, ctx->fhandle,
			     ctx->io_size, ctx->io_offset, 0, ctx->read_iov, 2, NULL,
			     fsssd_runtime_read_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_read failed: %d\n", rc);
		ctx->rc = rc;
		fsssd_runtime_release_file(ctx);
	}
}

static void
fsssd_runtime_open_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
			    struct spdk_fsdev_file_handle *fhandle)
{
	struct fsssd_runtime_ctx *ctx = cb_arg;

	SPDK_NOTICELOG("open complete status=%d\n", status);
	if (status != 0) {
		ctx->rc = status;
		fsssd_runtime_forget_file(ctx);
		return;
	}

	ctx->fhandle = fhandle;
	fsssd_runtime_write_file(ctx);
}

static void
fsssd_runtime_open_file(struct fsssd_runtime_ctx *ctx)
{
	int rc;

	rc = spdk_fsdev_fopen(ctx->desc, ctx->ch, ctx->unique++, ctx->file, O_RDWR,
			      fsssd_runtime_open_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_fopen failed: %d\n", rc);
		ctx->rc = rc;
		fsssd_runtime_forget_file(ctx);
	}
}

static void
fsssd_runtime_create_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
			      struct spdk_fsdev_file_object *fobject,
			      const struct spdk_fsdev_file_attr *attr,
			      struct spdk_fsdev_file_handle *fhandle)
{
	struct fsssd_runtime_ctx *ctx = cb_arg;

	SPDK_NOTICELOG("create complete status=%d\n", status);
	if (status != 0) {
		ctx->rc = status;
		fsssd_runtime_umount(ctx);
		return;
	}

	ctx->file = fobject;
	ctx->fhandle = fhandle;
	SPDK_NOTICELOG("created file attr: ino=%" PRIu64 " mode=0%o size=%" PRIu64 "\n",
		       attr->ino, attr->mode, attr->size);
	fsssd_runtime_write_file(ctx);
}

static void
fsssd_runtime_create_file(struct fsssd_runtime_ctx *ctx)
{
	int rc;

	SPDK_NOTICELOG("create file %s\n", ctx->file_name);
	rc = spdk_fsdev_create(ctx->desc, ctx->ch, ctx->unique++, ctx->root, ctx->file_name,
			       S_IFREG | 0644, O_RDWR, 0, 0, 0,
			       fsssd_runtime_create_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_create failed: %d\n", rc);
		ctx->rc = rc;
		fsssd_runtime_umount(ctx);
	}
}

static void
fsssd_runtime_lookup_complete(void *cb_arg, struct spdk_io_channel *ch, int status,
			      struct spdk_fsdev_file_object *fobject,
			      const struct spdk_fsdev_file_attr *attr)
{
	struct fsssd_runtime_ctx *ctx = cb_arg;

	SPDK_NOTICELOG("lookup complete status=%d\n", status);
	if (status != 0) {
		if (status == -ENOENT) {
			SPDK_NOTICELOG("lookup returned ENOENT; trying create for file %s\n", ctx->file_name);
			fsssd_runtime_create_file(ctx);
		} else {
			ctx->rc = status;
			fsssd_runtime_umount(ctx);
		}
		return;
	}

	ctx->file = fobject;
	SPDK_NOTICELOG("file attr: ino=%" PRIu64 " mode=0%o size=%" PRIu64 "\n",
		       attr->ino, attr->mode, attr->size);
	fsssd_runtime_open_file(ctx);
}

static void
fsssd_runtime_lookup_file(struct fsssd_runtime_ctx *ctx)
{
	int rc;

	rc = spdk_fsdev_lookup(ctx->desc, ctx->ch, ctx->unique++, ctx->root, ctx->file_name,
			       fsssd_runtime_lookup_complete, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_fsdev_lookup failed: %d\n", rc);
		ctx->rc = rc;
		fsssd_runtime_umount(ctx);
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
	if (ctx->file_name != NULL) {
		fsssd_runtime_lookup_file(ctx);
	} else {
		fsssd_runtime_umount(ctx);
	}
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
	if (ctx->io_offset % FSSSD_NFS_PAGE_SIZE != 0 || ctx->io_size % FSSSD_NFS_PAGE_SIZE != 0) {
		SPDK_ERRLOG("offset and size must be %u-byte aligned\n", FSSSD_NFS_PAGE_SIZE);
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

	rc = spdk_app_parse_args(argc, argv, &opts, "f:F:N:P:O:S:w:x", NULL,
				 fsssd_runtime_parse_arg, fsssd_runtime_usage);
	if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	rc = spdk_app_start(&opts, fsssd_runtime_start, &g_ctx);
	spdk_app_fini();

	return rc;
}
