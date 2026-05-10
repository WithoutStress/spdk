#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include "fsssd_transport.h"

struct fsssd_transport {
	char *name;
	char *device;
	uint32_t nsid;
};

struct fsssd_transport *
fsssd_transport_create(const struct fsssd_transport_opts *opts)
{
	struct fsssd_transport *transport;

	if (opts == NULL || opts->name == NULL || opts->device == NULL) {
		return NULL;
	}

	transport = calloc(1, sizeof(*transport));
	if (transport == NULL) {
		return NULL;
	}

	transport->name = strdup(opts->name);
	transport->device = strdup(opts->device);
	transport->nsid = opts->nsid ? opts->nsid : 1;
	if (transport->name == NULL || transport->device == NULL) {
		fsssd_transport_destroy(transport);
		return NULL;
	}

	return transport;
}

void
fsssd_transport_destroy(struct fsssd_transport *transport)
{
	if (transport == NULL) {
		return;
	}

	free(transport->name);
	free(transport->device);
	free(transport);
}

int
fsssd_transport_submit(struct fsssd_transport *transport, const struct fsssd_request *req,
		       struct fsssd_response *rsp)
{
	if (transport == NULL || req == NULL || rsp == NULL) {
		return -EINVAL;
	}

	memset(rsp, 0, sizeof(*rsp));
	SPDK_DEBUGLOG(fsdev_fsssd, "transport %s device %s nsid %u opcode 0x%x is not implemented\n",
		      transport->name, transport->device, transport->nsid, req->opcode);

	return -ENOTSUP;
}

const char *
fsssd_transport_get_name(const struct fsssd_transport *transport)
{
	return transport->name;
}

const char *
fsssd_transport_get_device(const struct fsssd_transport *transport)
{
	return transport->device;
}

uint32_t
fsssd_transport_get_nsid(const struct fsssd_transport *transport)
{
	return transport->nsid;
}

SPDK_LOG_REGISTER_COMPONENT(fsdev_fsssd)
