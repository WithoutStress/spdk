#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include "fsssd_client.h"

#define FSSSD_DEFAULT_MAX_WRITE 0x00020000

struct fsssd_client {
	struct fsssd_transport *transport;
	uint32_t max_write;
	bool writeback_cache_enabled;
};

static void
fsssd_attr_from_rsp(struct fsssd_attr *attr, uint64_t ino)
{
	memset(attr, 0, sizeof(*attr));
	attr->ino = ino;
	attr->mode = S_IFREG | 0644;
	attr->nlink = 1;
	attr->blksize = 4096;
}

static void
fsssd_attr_from_wire(struct fsssd_attr *attr, const struct fsssd_nfs_fattr *wire, uint64_t ino)
{
	memset(attr, 0, sizeof(*attr));
	attr->ino = wire->fileid ? wire->fileid : ino;
	attr->size = wire->size;
	attr->blocks = wire->used / 512;
	attr->atime = wire->atime.tv_sec;
	attr->mtime = wire->mtime.tv_sec;
	attr->ctime = wire->ctime.tv_sec;
	attr->mode = wire->mode;
	attr->nlink = wire->nlink;
	attr->uid = wire->uid;
	attr->gid = wire->gid;
	attr->blksize = FSSSD_NFS_PAGE_SIZE;
}

static void
fsssd_statfs_from_wire(struct fsssd_statfs *statfs, const struct fsssd_nfs_fsstat *wire)
{
	memset(statfs, 0, sizeof(*statfs));
	statfs->blocks = wire->tbytes / FSSSD_NFS_PAGE_SIZE;
	statfs->bfree = wire->fbytes / FSSSD_NFS_PAGE_SIZE;
	statfs->bavail = wire->abytes / FSSSD_NFS_PAGE_SIZE;
	statfs->files = wire->tfiles;
	statfs->ffree = wire->ffiles;
	statfs->bsize = FSSSD_NFS_PAGE_SIZE;
	statfs->namelen = FSSSD_NFS_MAX_NAME_LEN;
	statfs->frsize = FSSSD_NFS_PAGE_SIZE;
}

struct fsssd_client *
fsssd_client_create(const struct fsssd_client_opts *opts)
{
	struct fsssd_transport_opts transport_opts = {};
	struct fsssd_client *client;

	if (opts == NULL || opts->name == NULL || opts->device == NULL) {
		return NULL;
	}

	client = calloc(1, sizeof(*client));
	if (client == NULL) {
		return NULL;
	}

	transport_opts.name = opts->name;
	transport_opts.device = opts->device;
	transport_opts.nsid = opts->nsid;
	client->transport = fsssd_transport_create(&transport_opts);
	if (client->transport == NULL) {
		free(client);
		return NULL;
	}

	client->max_write = opts->max_write ? opts->max_write : FSSSD_DEFAULT_MAX_WRITE;
	client->writeback_cache_enabled = opts->writeback_cache_enabled;

	return client;
}

void
fsssd_client_destroy(struct fsssd_client *client)
{
	if (client == NULL) {
		return;
	}

	fsssd_transport_destroy(client->transport);
	free(client);
}

int
fsssd_client_mount(struct fsssd_client *client, uint64_t *root_ino, struct fsssd_attr *attr)
{
	struct fsssd_request req = {};
	struct fsssd_response rsp = {};
	int rc;

	if (client == NULL || root_ino == NULL || attr == NULL) {
		return -EINVAL;
	}

	req.opcode = fsssd_cmd_nfs_mnt;
	rc = fsssd_transport_submit(client->transport, &req, &rsp);
	if (rc != 0) {
		return rc;
	}

	*root_ino = rsp.result ? rsp.result : 1;
	fsssd_attr_from_rsp(attr, *root_ino);
	attr->mode = S_IFDIR | 0755;

	return 0;
}

int
fsssd_client_getattr(struct fsssd_client *client, uint64_t ino, struct fsssd_attr *attr)
{
	struct fsssd_request req = {};
	struct fsssd_response rsp = {};
	struct fsssd_nfs_fattr wire_attr = {};
	int rc;

	if (client == NULL || attr == NULL) {
		return -EINVAL;
	}

	req.opcode = fsssd_cmd_nfs_getattr;
	req.handle = ino;
	req.payload = &wire_attr;
	req.payload_len = sizeof(wire_attr);
	rc = fsssd_transport_submit(client->transport, &req, &rsp);
	if (rc != 0) {
		return rc;
	}

	fsssd_attr_from_wire(attr, &wire_attr, ino);

	return 0;
}

int
fsssd_client_lookup(struct fsssd_client *client, uint64_t parent_ino, const char *name,
		    uint64_t *ino, struct fsssd_attr *attr)
{
	struct fsssd_request req = {};
	struct fsssd_response rsp = {};
	int rc;

	if (client == NULL || name == NULL || ino == NULL || attr == NULL) {
		return -EINVAL;
	}

	req.opcode = fsssd_cmd_nfs_lookup;
	req.handle = parent_ino;
	req.name = name;
	rc = fsssd_transport_submit(client->transport, &req, &rsp);
	if (rc != 0) {
		return rc;
	}

	*ino = rsp.result;
	fsssd_attr_from_rsp(attr, *ino);

	return 0;
}

int
fsssd_client_statfs(struct fsssd_client *client, uint64_t ino, struct fsssd_statfs *statfs)
{
	struct fsssd_request req = {};
	struct fsssd_response rsp = {};
	struct fsssd_nfs_fsstat wire_statfs = {};
	int rc;

	if (client == NULL || statfs == NULL) {
		return -EINVAL;
	}

	req.opcode = fsssd_cmd_nfs_fsstat;
	req.handle = ino;
	req.payload = &wire_statfs;
	req.payload_len = sizeof(wire_statfs);
	rc = fsssd_transport_submit(client->transport, &req, &rsp);
	if (rc != 0) {
		return rc;
	}

	fsssd_statfs_from_wire(statfs, &wire_statfs);

	return 0;
}

int
fsssd_client_read(struct fsssd_client *client, uint64_t ino, uint64_t offset, size_t size,
		  struct iovec *iov, uint32_t iovcnt, uint32_t *data_size)
{
	struct fsssd_request req = {};
	struct fsssd_response rsp = {};
	int rc;

	if (client == NULL || iov == NULL || iovcnt == 0 || data_size == NULL) {
		return -EINVAL;
	}

	req.opcode = fsssd_cmd_nfs_read;
	req.handle = ino;
	req.offset = offset;
	req.size = size;
	req.iov = iov;
	req.iovcnt = iovcnt;
	rc = fsssd_transport_submit(client->transport, &req, &rsp);
	if (rc != 0) {
		return rc;
	}

	*data_size = rsp.data_size;

	return 0;
}

int
fsssd_client_write(struct fsssd_client *client, uint64_t ino, uint64_t offset, size_t size,
		   const struct iovec *iov, uint32_t iovcnt, uint32_t *data_size)
{
	struct fsssd_request req = {};
	struct fsssd_response rsp = {};
	int rc;

	if (client == NULL || iov == NULL || iovcnt == 0 || data_size == NULL) {
		return -EINVAL;
	}

	req.opcode = fsssd_cmd_nfs_write;
	req.handle = ino;
	req.offset = offset;
	req.size = size;
	req.iov = (struct iovec *)iov;
	req.iovcnt = iovcnt;
	rc = fsssd_transport_submit(client->transport, &req, &rsp);
	if (rc != 0) {
		return rc;
	}

	*data_size = rsp.data_size;

	return 0;
}

const char *
fsssd_client_get_device(const struct fsssd_client *client)
{
	return fsssd_transport_get_device(client->transport);
}

uint32_t
fsssd_client_get_nsid(const struct fsssd_client *client)
{
	return fsssd_transport_get_nsid(client->transport);
}

uint32_t
fsssd_client_get_max_write(const struct fsssd_client *client)
{
	return client->max_write;
}

bool
fsssd_client_get_writeback_cache_enabled(const struct fsssd_client *client)
{
	return client->writeback_cache_enabled;
}
