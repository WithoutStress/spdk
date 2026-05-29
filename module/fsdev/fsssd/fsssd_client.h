#ifndef FSSSD_CLIENT_H
#define FSSSD_CLIENT_H

#include "spdk/stdinc.h"
#include "fsssd_transport.h"

struct fsssd_client;
struct fsssd_client_channel;

struct fsssd_client_opts {
	const char *name;
	const char *device;
	uint32_t nsid;
	uint32_t max_write;
	bool writeback_cache_enabled;
};

struct fsssd_attr {
	uint64_t ino;
	uint64_t size;
	uint64_t blocks;
	uint64_t atime;
	uint64_t mtime;
	uint64_t ctime;
	uint32_t mode;
	uint32_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint32_t blksize;
};

struct fsssd_statfs {
	uint64_t blocks;
	uint64_t bfree;
	uint64_t bavail;
	uint64_t files;
	uint64_t ffree;
	uint32_t bsize;
	uint32_t namelen;
	uint32_t frsize;
};

struct fsssd_client *fsssd_client_create(const struct fsssd_client_opts *opts);
void fsssd_client_destroy(struct fsssd_client *client);
struct fsssd_client_channel *fsssd_client_channel_create(struct fsssd_client *client);
void fsssd_client_channel_destroy(struct fsssd_client_channel *channel);
int fsssd_client_channel_poll(struct fsssd_client_channel *channel);
int fsssd_client_submit_async(struct fsssd_client *client, struct fsssd_client_channel *channel,
			      const struct fsssd_request *req,
			      fsssd_transport_complete_cb cb_fn, void *cb_arg);
void fsssd_client_attr_from_result(struct fsssd_attr *attr, uint64_t ino);
void fsssd_client_attr_from_wire(struct fsssd_attr *attr, const struct fsssd_nfs_fattr *wire,
				 uint64_t ino);
void fsssd_client_statfs_from_wire(struct fsssd_statfs *statfs,
				   const struct fsssd_nfs_fsstat *wire);
const char *fsssd_client_get_device(const struct fsssd_client *client);
uint32_t fsssd_client_get_nsid(const struct fsssd_client *client);
uint32_t fsssd_client_get_max_write(const struct fsssd_client *client);
bool fsssd_client_get_writeback_cache_enabled(const struct fsssd_client *client);

#endif
