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

struct fsssd_client_channel {
	struct fsssd_transport_channel *transport_channel;
};

void
fsssd_client_attr_from_result(struct fsssd_attr *attr, uint64_t ino)
{
	memset(attr, 0, sizeof(*attr));
	attr->ino = ino;
	attr->mode = S_IFREG | 0644;
	attr->nlink = 1;
	attr->blksize = 4096;
}

void
fsssd_client_attr_from_wire(struct fsssd_attr *attr, const struct fsssd_nfs_fattr *wire,
			    uint64_t ino)
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

void
fsssd_client_statfs_from_wire(struct fsssd_statfs *statfs, const struct fsssd_nfs_fsstat *wire)
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

struct fsssd_client_channel *
fsssd_client_channel_create(struct fsssd_client *client)
{
	struct fsssd_client_channel *channel;

	if (client == NULL) {
		return NULL;
	}

	channel = calloc(1, sizeof(*channel));
	if (channel == NULL) {
		return NULL;
	}

	channel->transport_channel = fsssd_transport_channel_create(client->transport);
	if (channel->transport_channel == NULL) {
		free(channel);
		return NULL;
	}

	return channel;
}

void
fsssd_client_channel_destroy(struct fsssd_client_channel *channel)
{
	if (channel == NULL) {
		return;
	}

	fsssd_transport_channel_destroy(channel->transport_channel);
	free(channel);
}

int
fsssd_client_channel_poll(struct fsssd_client_channel *channel)
{
	if (channel == NULL) {
		return -EINVAL;
	}

	return fsssd_transport_channel_poll(channel->transport_channel);
}

int
fsssd_client_submit_async(struct fsssd_client *client, struct fsssd_client_channel *channel,
			  const struct fsssd_request *req,
			  fsssd_transport_complete_cb cb_fn, void *cb_arg)
{
	if (client == NULL || channel == NULL) {
		return -EINVAL;
	}

	return fsssd_transport_submit_async(client->transport, channel->transport_channel, req,
					   cb_fn, cb_arg);
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
