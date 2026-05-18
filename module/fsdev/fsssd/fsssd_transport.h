#ifndef FSSSD_TRANSPORT_H
#define FSSSD_TRANSPORT_H

#include "spdk/stdinc.h"
#include "fsssd_proto.h"

struct fsssd_transport;
struct fsssd_transport_channel;

struct fsssd_transport_opts {
	const char *name;
	const char *device;
	uint32_t nsid;
};

struct fsssd_request {
	enum fsssd_nfs_opcode opcode;
	uint64_t handle;
	const char *name;
	uint16_t mode;
	uint64_t offset;
	size_t size;
	struct iovec *iov;
	uint32_t iovcnt;
	void *payload;
	uint32_t payload_len;
};

struct fsssd_response {
	uint64_t result;
	uint32_t data_size;
};

typedef void (*fsssd_transport_complete_cb)(void *cb_arg, int status,
		const struct fsssd_response *rsp);

struct fsssd_transport *fsssd_transport_create(const struct fsssd_transport_opts *opts);
void fsssd_transport_destroy(struct fsssd_transport *transport);
struct fsssd_transport_channel *fsssd_transport_channel_create(struct fsssd_transport *transport);
void fsssd_transport_channel_destroy(struct fsssd_transport_channel *channel);
int fsssd_transport_channel_poll(struct fsssd_transport_channel *channel);
int fsssd_transport_submit(struct fsssd_transport *transport, const struct fsssd_request *req,
			   struct fsssd_response *rsp);
int fsssd_transport_submit_async(struct fsssd_transport *transport,
				 struct fsssd_transport_channel *channel,
				 const struct fsssd_request *req,
				 fsssd_transport_complete_cb cb_fn, void *cb_arg);
const char *fsssd_transport_get_name(const struct fsssd_transport *transport);
const char *fsssd_transport_get_device(const struct fsssd_transport *transport);
uint32_t fsssd_transport_get_nsid(const struct fsssd_transport *transport);

#endif
