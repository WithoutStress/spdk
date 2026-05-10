#ifndef SPDK_FSDEV_FSSSD_H
#define SPDK_FSDEV_FSSSD_H

#include "spdk/stdinc.h"
#include "spdk/fsdev_module.h"

struct spdk_fsdev_fsssd_opts {
	char *device;
	uint32_t nsid;
	uint32_t max_write;
	bool writeback_cache_enabled;
};

typedef void (*spdk_delete_fsssd_fsdev_complete)(void *cb_arg, int fsdeverrno);

void spdk_fsdev_fsssd_get_default_opts(struct spdk_fsdev_fsssd_opts *opts);
int spdk_fsdev_fsssd_create(struct spdk_fsdev **fsdev, const char *name,
			    const struct spdk_fsdev_fsssd_opts *opts);
void spdk_fsdev_fsssd_delete(const char *name, spdk_delete_fsssd_fsdev_complete cb_fn,
			     void *cb_arg);

#endif
