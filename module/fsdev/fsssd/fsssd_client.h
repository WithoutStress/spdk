#ifndef FSSSD_CLIENT_H
#define FSSSD_CLIENT_H

#include "spdk/stdinc.h"
#include "fsssd_transport.h"

struct fsssd_client;

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
int fsssd_client_mount(struct fsssd_client *client, uint64_t *root_ino, struct fsssd_attr *attr);
int fsssd_client_getattr(struct fsssd_client *client, uint64_t ino, struct fsssd_attr *attr);
int fsssd_client_lookup(struct fsssd_client *client, uint64_t parent_ino, const char *name,
			uint64_t *ino, struct fsssd_attr *attr);
int fsssd_client_create_file(struct fsssd_client *client, uint64_t parent_ino, const char *name,
			     uint16_t mode, uint64_t *ino, struct fsssd_attr *attr);
int fsssd_client_statfs(struct fsssd_client *client, uint64_t ino, struct fsssd_statfs *statfs);
int fsssd_client_read(struct fsssd_client *client, uint64_t ino, uint64_t offset, size_t size,
		      struct iovec *iov, uint32_t iovcnt, uint32_t *data_size);
int fsssd_client_write(struct fsssd_client *client, uint64_t ino, uint64_t offset, size_t size,
		       const struct iovec *iov, uint32_t iovcnt, uint32_t *data_size);
const char *fsssd_client_get_device(const struct fsssd_client *client);
uint32_t fsssd_client_get_nsid(const struct fsssd_client *client);
uint32_t fsssd_client_get_max_write(const struct fsssd_client *client);
bool fsssd_client_get_writeback_cache_enabled(const struct fsssd_client *client);

#endif
