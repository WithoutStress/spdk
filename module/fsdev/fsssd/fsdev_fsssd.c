#include "spdk/stdinc.h"
#include "spdk/fsdev.h"
#include "spdk/fsdev_module.h"
#include "spdk/json.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"

#include "fsdev_fsssd.h"
#include "fsssd_client.h"

#define FSSSD_DEFAULT_MAX_WRITE 0x00020000
#define IO_STATUS_ASYNC INT_MIN

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

struct spdk_fsdev_file_object {
	uint64_t ino;
	uint64_t refcount;
	struct spdk_fsdev_file_object *parent;
	TAILQ_ENTRY(spdk_fsdev_file_object) link;
	TAILQ_HEAD(, spdk_fsdev_file_object) leafs;
	struct spdk_spinlock lock;
};

struct spdk_fsdev_file_handle {
	struct spdk_fsdev_file_object *fobject;
	TAILQ_ENTRY(spdk_fsdev_file_handle) link;
};

struct fsssd_fsdev {
	struct spdk_fsdev fsdev;
	struct fsssd_client *client;
	struct spdk_fsdev_file_object *root;
	struct spdk_fsdev_mount_opts mount_opts;
	TAILQ_ENTRY(fsssd_fsdev) tailq;
};

struct fsssd_fsdev_io {
	struct spdk_fsdev_file_object *parent;
	struct fsssd_nfs_fattr wire_attr;
	struct fsssd_nfs_fsstat wire_statfs;
};

struct fsssd_channel_entry {
	struct fsssd_fsdev *vfsdev;
	struct fsssd_client_channel *client_channel;
	TAILQ_ENTRY(fsssd_channel_entry) link;
};

struct fsssd_io_channel {
	struct spdk_poller *poller;
	TAILQ_HEAD(, fsssd_channel_entry) channels;
};

static TAILQ_HEAD(, fsssd_fsdev) g_fsssd_fsdev_head = TAILQ_HEAD_INITIALIZER(g_fsssd_fsdev_head);

static inline struct fsssd_fsdev *
fsdev_to_fsssd(struct spdk_fsdev *fsdev)
{
	return SPDK_CONTAINEROF(fsdev, struct fsssd_fsdev, fsdev);
}

static inline struct fsssd_fsdev_io *
fsdev_to_fsssd_io(const struct spdk_fsdev_io *fsdev_io)
{
	return (struct fsssd_fsdev_io *)fsdev_io->driver_ctx;
}

static struct spdk_fsdev_file_object *
fsssd_file_object_create(struct spdk_fsdev_file_object *parent, uint64_t ino)
{
	struct spdk_fsdev_file_object *fobject;

	fobject = calloc(1, sizeof(*fobject));
	if (fobject == NULL) {
		return NULL;
	}

	fobject->ino = ino;
	fobject->refcount = 1;
	fobject->parent = parent;
	TAILQ_INIT(&fobject->leafs);
	spdk_spin_init(&fobject->lock);

	if (parent != NULL) {
		spdk_spin_lock(&parent->lock);
		parent->refcount++;
		TAILQ_INSERT_TAIL(&parent->leafs, fobject, link);
		spdk_spin_unlock(&parent->lock);
	}

	return fobject;
}

static void
fsssd_file_object_ref(struct spdk_fsdev_file_object *fobject)
{
	spdk_spin_lock(&fobject->lock);
	fobject->refcount++;
	spdk_spin_unlock(&fobject->lock);
}

static void
fsssd_file_object_unref(struct spdk_fsdev_file_object *fobject, uint64_t count)
{
	struct spdk_fsdev_file_object *parent;

	spdk_spin_lock(&fobject->lock);
	assert(fobject->refcount >= count);
	fobject->refcount -= count;
	if (fobject->refcount != 0) {
		spdk_spin_unlock(&fobject->lock);
		return;
	}
	spdk_spin_unlock(&fobject->lock);

	parent = fobject->parent;
	if (parent != NULL) {
		spdk_spin_lock(&parent->lock);
		TAILQ_REMOVE(&parent->leafs, fobject, link);
		spdk_spin_unlock(&parent->lock);
		fsssd_file_object_unref(parent, 1);
	}

	spdk_spin_destroy(&fobject->lock);
	free(fobject);
}

static void
fsssd_file_object_free_leafs(struct spdk_fsdev_file_object *fobject)
{
	while (!TAILQ_EMPTY(&fobject->leafs)) {
		struct spdk_fsdev_file_object *leaf = TAILQ_FIRST(&fobject->leafs);

		fsssd_file_object_free_leafs(leaf);
		fsssd_file_object_unref(leaf, leaf->refcount);
	}
}

static struct spdk_fsdev_file_handle *
fsssd_file_handle_create(struct spdk_fsdev_file_object *fobject)
{
	struct spdk_fsdev_file_handle *fhandle;

	fhandle = calloc(1, sizeof(*fhandle));
	if (fhandle == NULL) {
		return NULL;
	}

	fhandle->fobject = fobject;
	fsssd_file_object_ref(fobject);

	return fhandle;
}

static void
fsssd_file_handle_delete(struct spdk_fsdev_file_handle *fhandle)
{
	fsssd_file_object_unref(fhandle->fobject, 1);
	free(fhandle);
}

static void
fsssd_attr_to_fsdev(const struct fsssd_attr *src, struct spdk_fsdev_file_attr *dst)
{
	memset(dst, 0, sizeof(*dst));
	dst->ino = src->ino;
	dst->size = src->size;
	dst->blocks = src->blocks;
	dst->atime = src->atime;
	dst->mtime = src->mtime;
	dst->ctime = src->ctime;
	dst->mode = src->mode;
	dst->nlink = src->nlink;
	dst->uid = src->uid;
	dst->gid = src->gid;
	dst->blksize = src->blksize;
}

static void
fsssd_statfs_to_fsdev(const struct fsssd_statfs *src, struct spdk_fsdev_file_statfs *dst)
{
	memset(dst, 0, sizeof(*dst));
	dst->blocks = src->blocks;
	dst->bfree = src->bfree;
	dst->bavail = src->bavail;
	dst->files = src->files;
	dst->ffree = src->ffree;
	dst->bsize = src->bsize;
	dst->namelen = src->namelen;
	dst->frsize = src->frsize;
}

static struct fsssd_client_channel *
fsssd_get_client_channel(struct spdk_io_channel *ch, struct fsssd_fsdev *vfsdev)
{
	struct fsssd_io_channel *fch = spdk_io_channel_get_ctx(ch);
	struct fsssd_channel_entry *entry;

	TAILQ_FOREACH(entry, &fch->channels, link) {
		if (entry->vfsdev == vfsdev) {
			return entry->client_channel;
		}
	}

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		return NULL;
	}

	entry->vfsdev = vfsdev;
	entry->client_channel = fsssd_client_channel_create(vfsdev->client);
	if (entry->client_channel == NULL) {
		free(entry);
		return NULL;
	}

	TAILQ_INSERT_TAIL(&fch->channels, entry, link);
	return entry->client_channel;
}

static void
fsssd_remove_client_channel(struct fsssd_io_channel *fch, struct fsssd_fsdev *vfsdev)
{
	struct fsssd_channel_entry *entry, *tmp;

	TAILQ_FOREACH_SAFE(entry, &fch->channels, link, tmp) {
		if (entry->vfsdev == vfsdev) {
			TAILQ_REMOVE(&fch->channels, entry, link);
			fsssd_client_channel_destroy(entry->client_channel);
			free(entry);
			return;
		}
	}
}

static int
fsssd_submit_async(struct spdk_io_channel *ch, struct fsssd_fsdev *vfsdev,
		   const struct fsssd_request *req,
		   fsssd_transport_complete_cb cb_fn, struct spdk_fsdev_io *fsdev_io)
{
	struct fsssd_client_channel *client_channel;

	client_channel = fsssd_get_client_channel(ch, vfsdev);
	if (client_channel == NULL) {
		return -ENOMEM;
	}

	return fsssd_client_submit_async(vfsdev->client, client_channel, req, cb_fn, fsdev_io);
}

static void
fsssd_mount_complete(void *cb_arg, int status, const struct fsssd_response *rsp)
{
	struct spdk_fsdev_io *fsdev_io = cb_arg;
	struct fsssd_fsdev *vfsdev = fsdev_to_fsssd(fsdev_io->fsdev);
	uint64_t root_ino;

	if (status == 0) {
		root_ino = rsp->result ? rsp->result : 1;

		if (vfsdev->root == NULL) {
			vfsdev->root = fsssd_file_object_create(NULL, root_ino);
			if (vfsdev->root == NULL) {
				status = -ENOMEM;
			}
		}
	}

	if (status == 0) {
		fsdev_io->u_out.mount.opts = fsdev_io->u_in.mount.opts;
		fsdev_io->u_out.mount.opts.max_write = vfsdev->mount_opts.max_write;
		fsdev_io->u_out.mount.opts.writeback_cache_enabled =
			vfsdev->mount_opts.writeback_cache_enabled;
		fsssd_file_object_ref(vfsdev->root);
		fsdev_io->u_out.mount.root_fobject = vfsdev->root;
	}

	spdk_fsdev_io_complete(fsdev_io, status);
}

static int
fsssd_mount(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct fsssd_fsdev *vfsdev = fsdev_to_fsssd(fsdev_io->fsdev);
	struct fsssd_request req = {};
	int rc;

	req.opcode = fsssd_cmd_nfs_mnt;
	rc = fsssd_submit_async(ch, vfsdev, &req, fsssd_mount_complete, fsdev_io);
	if (rc != 0) {
		return rc;
	}

	return IO_STATUS_ASYNC;
}

static int
fsssd_umount(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct fsssd_fsdev *vfsdev = fsdev_to_fsssd(fsdev_io->fsdev);

	if (vfsdev->root != NULL) {
		fsssd_file_object_free_leafs(vfsdev->root);
		fsssd_file_object_unref(vfsdev->root, 1);
	}

	return 0;
}

static void
fsssd_lookup_complete(void *cb_arg, int status, const struct fsssd_response *rsp)
{
	struct spdk_fsdev_io *fsdev_io = cb_arg;
	struct fsssd_fsdev_io *fsssd_io = fsdev_to_fsssd_io(fsdev_io);
	struct fsssd_attr attr;

	if (status == 0) {
		fsdev_io->u_out.lookup.fobject = fsssd_file_object_create(fsssd_io->parent, rsp->result);
		if (fsdev_io->u_out.lookup.fobject == NULL) {
			status = -ENOMEM;
		}
	}

	if (status == 0) {
		fsssd_client_attr_from_result(&attr, rsp->result);
		fsssd_attr_to_fsdev(&attr, &fsdev_io->u_out.lookup.attr);
	}

	spdk_fsdev_io_complete(fsdev_io, status);
}

static int
fsssd_lookup(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct fsssd_fsdev *vfsdev = fsdev_to_fsssd(fsdev_io->fsdev);
	struct fsssd_fsdev_io *fsssd_io = fsdev_to_fsssd_io(fsdev_io);
	struct spdk_fsdev_file_object *parent = fsdev_io->u_in.lookup.parent_fobject;
	struct fsssd_request req = {};
	int rc;

	if (parent == NULL) {
		parent = vfsdev->root;
	}

	if (parent == NULL) {
		return -EINVAL;
	}

	fsssd_io->parent = parent;
	req.opcode = fsssd_cmd_nfs_lookup;
	req.handle = parent->ino;
	req.name = fsdev_io->u_in.lookup.name;
	rc = fsssd_submit_async(ch, vfsdev, &req, fsssd_lookup_complete, fsdev_io);
	if (rc != 0) {
		return rc;
	}

	return IO_STATUS_ASYNC;
}

static int
fsssd_forget(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.forget.fobject;

	if (fobject == NULL) {
		return -EINVAL;
	}

	fsssd_file_object_unref(fobject, fsdev_io->u_in.forget.nlookup);

	return 0;
}

static void
fsssd_getattr_complete(void *cb_arg, int status, const struct fsssd_response *rsp)
{
	struct spdk_fsdev_io *fsdev_io = cb_arg;
	struct fsssd_fsdev_io *fsssd_io = fsdev_to_fsssd_io(fsdev_io);
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.getattr.fobject;
	struct fsssd_attr attr;

	UNUSED(rsp);

	if (status == 0) {
		fsssd_client_attr_from_wire(&attr, &fsssd_io->wire_attr, fobject->ino);
		fsssd_attr_to_fsdev(&attr, &fsdev_io->u_out.getattr.attr);
	}

	spdk_fsdev_io_complete(fsdev_io, status);
}

static int
fsssd_getattr(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct fsssd_fsdev *vfsdev = fsdev_to_fsssd(fsdev_io->fsdev);
	struct fsssd_fsdev_io *fsssd_io = fsdev_to_fsssd_io(fsdev_io);
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.getattr.fobject;
	struct fsssd_request req = {};
	int rc;

	if (fobject == NULL) {
		return -EINVAL;
	}

	memset(&fsssd_io->wire_attr, 0, sizeof(fsssd_io->wire_attr));
	req.opcode = fsssd_cmd_nfs_getattr;
	req.handle = fobject->ino;
	req.payload = &fsssd_io->wire_attr;
	req.payload_len = sizeof(fsssd_io->wire_attr);
	rc = fsssd_submit_async(ch, vfsdev, &req, fsssd_getattr_complete, fsdev_io);
	if (rc != 0) {
		return rc;
	}

	return IO_STATUS_ASYNC;
}

static int
fsssd_open(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.open.fobject;

	if (fobject == NULL) {
		return -EINVAL;
	}

	fsdev_io->u_out.open.fhandle = fsssd_file_handle_create(fobject);
	if (fsdev_io->u_out.open.fhandle == NULL) {
		return -ENOMEM;
	}

	return 0;
}

static void
fsssd_create_complete(void *cb_arg, int status, const struct fsssd_response *rsp)
{
	struct spdk_fsdev_io *fsdev_io = cb_arg;
	struct fsssd_fsdev_io *fsssd_io = fsdev_to_fsssd_io(fsdev_io);
	struct fsssd_attr attr;

	if (status == 0) {
		fsdev_io->u_out.create.fobject = fsssd_file_object_create(fsssd_io->parent, rsp->result);
		if (fsdev_io->u_out.create.fobject == NULL) {
			status = -ENOMEM;
		}
	}

	if (status == 0) {
		fsdev_io->u_out.create.fhandle = fsssd_file_handle_create(fsdev_io->u_out.create.fobject);
		if (fsdev_io->u_out.create.fhandle == NULL) {
			fsssd_file_object_unref(fsdev_io->u_out.create.fobject, 1);
			status = -ENOMEM;
		}
	}

	if (status == 0) {
		fsssd_client_attr_from_result(&attr, rsp->result);
		fsssd_attr_to_fsdev(&attr, &fsdev_io->u_out.create.attr);
	}

	spdk_fsdev_io_complete(fsdev_io, status);
}

static int
fsssd_create(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct fsssd_fsdev *vfsdev = fsdev_to_fsssd(fsdev_io->fsdev);
	struct fsssd_fsdev_io *fsssd_io = fsdev_to_fsssd_io(fsdev_io);
	struct spdk_fsdev_file_object *parent = fsdev_io->u_in.create.parent_fobject;
	struct fsssd_request req = {};
	int rc;

	if (parent == NULL) {
		parent = vfsdev->root;
	}

	if (parent == NULL || fsdev_io->u_in.create.name == NULL) {
		return -EINVAL;
	}

	fsssd_io->parent = parent;
	req.opcode = fsssd_cmd_nfs_create;
	req.handle = parent->ino;
	req.name = fsdev_io->u_in.create.name;
	req.mode = fsdev_io->u_in.create.mode;
	rc = fsssd_submit_async(ch, vfsdev, &req, fsssd_create_complete, fsdev_io);
	if (rc != 0) {
		return rc;
	}

	return IO_STATUS_ASYNC;
}

static int
fsssd_release(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct spdk_fsdev_file_handle *fhandle = fsdev_io->u_in.release.fhandle;

	if (fhandle == NULL) {
		return -EINVAL;
	}

	fsssd_file_handle_delete(fhandle);

	return 0;
}

static void
fsssd_read_complete(void *cb_arg, int status, const struct fsssd_response *rsp)
{
	struct spdk_fsdev_io *fsdev_io = cb_arg;

	if (status == 0) {
		fsdev_io->u_out.read.data_size = rsp->data_size;
	}

	spdk_fsdev_io_complete(fsdev_io, status);
}

static int
fsssd_read(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct fsssd_fsdev *vfsdev = fsdev_to_fsssd(fsdev_io->fsdev);
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.read.fobject;
	struct fsssd_request req = {};
	int rc;

	if (fobject == NULL || fsdev_io->u_in.read.fhandle == NULL) {
		return -EINVAL;
	}

	req.opcode = fsssd_cmd_nfs_read;
	req.handle = fobject->ino;
	req.offset = fsdev_io->u_in.read.offs;
	req.size = fsdev_io->u_in.read.size;
	req.iov = fsdev_io->u_in.read.iov;
	req.iovcnt = fsdev_io->u_in.read.iovcnt;
	rc = fsssd_submit_async(ch, vfsdev, &req, fsssd_read_complete, fsdev_io);
	if (rc != 0) {
		return rc;
	}

	return IO_STATUS_ASYNC;
}

static void
fsssd_write_complete(void *cb_arg, int status, const struct fsssd_response *rsp)
{
	struct spdk_fsdev_io *fsdev_io = cb_arg;

	if (status == 0) {
		fsdev_io->u_out.write.data_size = rsp->data_size;
	}

	spdk_fsdev_io_complete(fsdev_io, status);
}

static int
fsssd_write(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct fsssd_fsdev *vfsdev = fsdev_to_fsssd(fsdev_io->fsdev);
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.write.fobject;
	struct fsssd_request req = {};
	int rc;

	if (fobject == NULL || fsdev_io->u_in.write.fhandle == NULL) {
		return -EINVAL;
	}

	req.opcode = fsssd_cmd_nfs_write;
	req.handle = fobject->ino;
	req.offset = fsdev_io->u_in.write.offs;
	req.size = fsdev_io->u_in.write.size;
	req.iov = (struct iovec *)fsdev_io->u_in.write.iov;
	req.iovcnt = fsdev_io->u_in.write.iovcnt;
	rc = fsssd_submit_async(ch, vfsdev, &req, fsssd_write_complete, fsdev_io);
	if (rc != 0) {
		return rc;
	}

	return IO_STATUS_ASYNC;
}

static void
fsssd_statfs_complete(void *cb_arg, int status, const struct fsssd_response *rsp)
{
	struct spdk_fsdev_io *fsdev_io = cb_arg;
	struct fsssd_fsdev_io *fsssd_io = fsdev_to_fsssd_io(fsdev_io);
	struct fsssd_statfs statfs;

	UNUSED(rsp);

	if (status == 0) {
		fsssd_client_statfs_from_wire(&statfs, &fsssd_io->wire_statfs);
		fsssd_statfs_to_fsdev(&statfs, &fsdev_io->u_out.statfs.statfs);
	}

	spdk_fsdev_io_complete(fsdev_io, status);
}

static int
fsssd_statfs(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	struct fsssd_fsdev *vfsdev = fsdev_to_fsssd(fsdev_io->fsdev);
	struct fsssd_fsdev_io *fsssd_io = fsdev_to_fsssd_io(fsdev_io);
	struct spdk_fsdev_file_object *fobject = fsdev_io->u_in.statfs.fobject;
	struct fsssd_request req = {};
	int rc;

	if (fobject == NULL) {
		return -EINVAL;
	}

	memset(&fsssd_io->wire_statfs, 0, sizeof(fsssd_io->wire_statfs));
	req.opcode = fsssd_cmd_nfs_fsstat;
	req.handle = fobject->ino;
	req.payload = &fsssd_io->wire_statfs;
	req.payload_len = sizeof(fsssd_io->wire_statfs);
	rc = fsssd_submit_async(ch, vfsdev, &req, fsssd_statfs_complete, fsdev_io);
	if (rc != 0) {
		return rc;
	}

	return IO_STATUS_ASYNC;
}

static int
fsssd_flush(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	return 0;
}

static int
fsssd_unsupported(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	return -ENOTSUP;
}

static int
fsssd_channel_poll(void *arg)
{
	struct fsssd_io_channel *fch = arg;
	struct fsssd_channel_entry *entry;
	int rc;
	int work_done = 0;

	TAILQ_FOREACH(entry, &fch->channels, link) {
		rc = fsssd_client_channel_poll(entry->client_channel);
		if (rc > 0) {
			work_done += rc;
		}
	}

	return work_done ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
fsssd_channel_create_cb(void *io_device, void *ctx_buf)
{
	struct fsssd_io_channel *fch = ctx_buf;

	TAILQ_INIT(&fch->channels);
	fch->poller = SPDK_POLLER_REGISTER(fsssd_channel_poll, fch, 0);
	if (fch->poller == NULL) {
		return -ENOMEM;
	}

	return 0;
}

static void
fsssd_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct fsssd_io_channel *fch = ctx_buf;
	struct fsssd_channel_entry *entry;

	spdk_poller_unregister(&fch->poller);
	while (!TAILQ_EMPTY(&fch->channels)) {
		entry = TAILQ_FIRST(&fch->channels);
		TAILQ_REMOVE(&fch->channels, entry, link);
		fsssd_client_channel_destroy(entry->client_channel);
		free(entry);
	}
}

static int
fsdev_fsssd_initialize(void)
{
	spdk_io_device_register(&g_fsssd_fsdev_head, fsssd_channel_create_cb, fsssd_channel_destroy_cb,
				sizeof(struct fsssd_io_channel), "fsssd_fsdev");

	return 0;
}

static void
fsdev_fsssd_finish_cb(void *arg)
{
}

static void
fsdev_fsssd_finish(void)
{
	spdk_io_device_unregister(&g_fsssd_fsdev_head, fsdev_fsssd_finish_cb);
}

static int
fsdev_fsssd_get_ctx_size(void)
{
	return sizeof(struct fsssd_fsdev_io);
}

static struct spdk_fsdev_module fsssd_fsdev_module = {
	.name = "fsssd",
	.module_init = fsdev_fsssd_initialize,
	.module_fini = fsdev_fsssd_finish,
	.get_ctx_size = fsdev_fsssd_get_ctx_size,
};

SPDK_FSDEV_MODULE_REGISTER(fsssd, &fsssd_fsdev_module);

static void
fsssd_remove_client_channel_iter(struct spdk_io_channel_iter *i)
{
	struct fsssd_fsdev *vfsdev = spdk_io_channel_iter_get_ctx(i);
	struct fsssd_io_channel *fch = spdk_io_channel_get_ctx(spdk_io_channel_iter_get_channel(i));

	fsssd_remove_client_channel(fch, vfsdev);
	spdk_for_each_channel_continue(i, 0);
}

static void
fsssd_remove_client_channel_done(struct spdk_io_channel_iter *i, int status)
{
	struct fsssd_fsdev *vfsdev = spdk_io_channel_iter_get_ctx(i);

	if (vfsdev->root != NULL) {
		fsssd_file_object_free_leafs(vfsdev->root);
		fsssd_file_object_unref(vfsdev->root, vfsdev->root->refcount);
	}

	fsssd_client_destroy(vfsdev->client);
	spdk_fsdev_destruct_done(&vfsdev->fsdev, status);
	free(vfsdev->fsdev.name);
	free(vfsdev);
}

static int
fsdev_fsssd_destruct(void *ctx)
{
	struct fsssd_fsdev *vfsdev = ctx;

	TAILQ_REMOVE(&g_fsssd_fsdev_head, vfsdev, tailq);
	spdk_for_each_channel(&g_fsssd_fsdev_head, fsssd_remove_client_channel_iter, vfsdev,
			      fsssd_remove_client_channel_done);

	return 1;
}

typedef int (*fsdev_op_handler_func)(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io);

static fsdev_op_handler_func fsssd_handlers[] = {
	[SPDK_FSDEV_IO_MOUNT] = fsssd_mount,
	[SPDK_FSDEV_IO_UMOUNT] = fsssd_umount,
	[SPDK_FSDEV_IO_LOOKUP] = fsssd_lookup,
	[SPDK_FSDEV_IO_FORGET] = fsssd_forget,
	[SPDK_FSDEV_IO_GETATTR] = fsssd_getattr,
	[SPDK_FSDEV_IO_OPEN] = fsssd_open,
	[SPDK_FSDEV_IO_CREATE] = fsssd_create,
	[SPDK_FSDEV_IO_READ] = fsssd_read,
	[SPDK_FSDEV_IO_WRITE] = fsssd_write,
	[SPDK_FSDEV_IO_STATFS] = fsssd_statfs,
	[SPDK_FSDEV_IO_RELEASE] = fsssd_release,
	[SPDK_FSDEV_IO_FLUSH] = fsssd_flush,
};

static void
fsdev_fsssd_submit_request(struct spdk_io_channel *ch, struct spdk_fsdev_io *fsdev_io)
{
	enum spdk_fsdev_io_type type = spdk_fsdev_io_get_type(fsdev_io);
	int status;

	assert(type >= 0 && type < __SPDK_FSDEV_IO_LAST);

	if (fsssd_handlers[type] == NULL) {
		status = fsssd_unsupported(ch, fsdev_io);
	} else {
		status = fsssd_handlers[type](ch, fsdev_io);
	}

	if (status != IO_STATUS_ASYNC) {
		spdk_fsdev_io_complete(fsdev_io, status);
	}
}

static struct spdk_io_channel *
fsdev_fsssd_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_fsssd_fsdev_head);
}

static void
fsdev_fsssd_write_config_json(struct spdk_fsdev *fsdev, struct spdk_json_write_ctx *w)
{
	struct fsssd_fsdev *vfsdev = fsdev_to_fsssd(fsdev);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "fsdev_fsssd_create");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", spdk_fsdev_get_name(&vfsdev->fsdev));
	spdk_json_write_named_string(w, "device", fsssd_client_get_device(vfsdev->client));
	spdk_json_write_named_uint32(w, "nsid", fsssd_client_get_nsid(vfsdev->client));
	spdk_json_write_named_uint32(w, "max_write", fsssd_client_get_max_write(vfsdev->client));
	spdk_json_write_named_bool(w, "enable_writeback_cache",
				 !!fsssd_client_get_writeback_cache_enabled(vfsdev->client));
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static const struct spdk_fsdev_fn_table fsssd_fn_table = {
	.destruct = fsdev_fsssd_destruct,
	.submit_request = fsdev_fsssd_submit_request,
	.get_io_channel = fsdev_fsssd_get_io_channel,
	.write_config_json = fsdev_fsssd_write_config_json,
};

void
spdk_fsdev_fsssd_get_default_opts(struct spdk_fsdev_fsssd_opts *opts)
{
	assert(opts != NULL);

	memset(opts, 0, sizeof(*opts));
	opts->nsid = 1;
	opts->max_write = FSSSD_DEFAULT_MAX_WRITE;
	opts->writeback_cache_enabled = false;
}

int
spdk_fsdev_fsssd_create(struct spdk_fsdev **fsdev, const char *name,
			const struct spdk_fsdev_fsssd_opts *opts)
{
	struct fsssd_client_opts client_opts = {};
	struct fsssd_fsdev *vfsdev;
	int rc;

	if (fsdev == NULL || name == NULL || opts == NULL || opts->device == NULL) {
		return -EINVAL;
	}

	vfsdev = calloc(1, sizeof(*vfsdev));
	if (vfsdev == NULL) {
		return -ENOMEM;
	}

	vfsdev->fsdev.name = strdup(name);
	if (vfsdev->fsdev.name == NULL) {
		free(vfsdev);
		return -ENOMEM;
	}

	client_opts.name = name;
	client_opts.device = opts->device;
	client_opts.nsid = opts->nsid;
	client_opts.max_write = opts->max_write;
	client_opts.writeback_cache_enabled = opts->writeback_cache_enabled;
	vfsdev->client = fsssd_client_create(&client_opts);
	if (vfsdev->client == NULL) {
		free(vfsdev->fsdev.name);
		free(vfsdev);
		return -ENOMEM;
	}

	vfsdev->mount_opts.max_write = opts->max_write ? opts->max_write : FSSSD_DEFAULT_MAX_WRITE;
	vfsdev->mount_opts.writeback_cache_enabled = opts->writeback_cache_enabled;
	vfsdev->fsdev.ctxt = vfsdev;
	vfsdev->fsdev.fn_table = &fsssd_fn_table;
	vfsdev->fsdev.module = &fsssd_fsdev_module;

	rc = spdk_fsdev_register(&vfsdev->fsdev);
	if (rc != 0) {
		fsssd_client_destroy(vfsdev->client);
		free(vfsdev->fsdev.name);
		free(vfsdev);
		return rc;
	}

	*fsdev = &vfsdev->fsdev;
	TAILQ_INSERT_TAIL(&g_fsssd_fsdev_head, vfsdev, tailq);

	return 0;
}

void
spdk_fsdev_fsssd_delete(const char *name, spdk_delete_fsssd_fsdev_complete cb_fn,
			void *cb_arg)
{
	int rc;

	rc = spdk_fsdev_unregister_by_name(name, &fsssd_fsdev_module, cb_fn, cb_arg);
	if (rc != 0) {
		cb_fn(cb_arg, rc);
	}
}
