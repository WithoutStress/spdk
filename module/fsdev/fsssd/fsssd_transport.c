#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/string.h"

#include "fsssd_transport.h"

struct fsssd_transport {
	char *name;
	char *device;
	uint32_t nsid;
	struct spdk_nvme_transport_id trid;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_qpair *qpair;
};

struct fsssd_nvme_completion {
	bool done;
	struct spdk_nvme_cpl cpl;
};

struct fsssd_transport_payload {
	void *buf;
	uint32_t len;
	void *copy_dst;
	bool copy_out;
	bool dma_allocated;
};

static int
fsssd_transport_parse_trid(struct spdk_nvme_transport_id *trid, const char *device)
{
	int rc;

	rc = spdk_nvme_transport_id_parse(trid, device);
	if (rc == 0) {
		return 0;
	}

	memset(trid, 0, sizeof(*trid));
	spdk_nvme_trid_populate_transport(trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(trid->traddr, sizeof(trid->traddr), "%s", device);

	return 0;
}

struct fsssd_transport *
fsssd_transport_create(const struct fsssd_transport_opts *opts)
{
	struct fsssd_transport *transport;
	int rc;

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

	rc = fsssd_transport_parse_trid(&transport->trid, transport->device);
	if (rc != 0) {
		fsssd_transport_destroy(transport);
		return NULL;
	}

	transport->ctrlr = spdk_nvme_connect(&transport->trid, NULL, 0);
	if (transport->ctrlr == NULL) {
		fsssd_transport_destroy(transport);
		return NULL;
	}

	transport->ns = spdk_nvme_ctrlr_get_ns(transport->ctrlr, transport->nsid);
	if (transport->ns == NULL || !spdk_nvme_ns_is_active(transport->ns)) {
		fsssd_transport_destroy(transport);
		return NULL;
	}

	transport->qpair = spdk_nvme_ctrlr_alloc_io_qpair(transport->ctrlr, NULL, 0);
	if (transport->qpair == NULL) {
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

	if (transport->qpair != NULL) {
		spdk_nvme_ctrlr_free_io_qpair(transport->qpair);
	}
	if (transport->ctrlr != NULL) {
		spdk_nvme_detach(transport->ctrlr);
	}
	free(transport->name);
	free(transport->device);
	free(transport);
}

static void
fsssd_transport_nvme_complete(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct fsssd_nvme_completion *completion = ctx;

	completion->cpl = *cpl;
	completion->done = true;
}

static int
fsssd_transport_wait_completion(struct fsssd_transport *transport,
				struct fsssd_nvme_completion *completion)
{
	uint64_t timeout_tsc;
	int32_t rc;

	timeout_tsc = spdk_get_ticks() + FSSSD_NVME_CMD_TIMEOUT_SEC * spdk_get_ticks_hz();
	while (!completion->done) {
		rc = spdk_nvme_qpair_process_completions(transport->qpair, 0);
		if (rc < 0) {
			return rc;
		}
		if (spdk_get_ticks() > timeout_tsc) {
			return -ETIMEDOUT;
		}
	}

	return spdk_nvme_cpl_is_error(&completion->cpl) ? -EIO : 0;
}

static int
fsssd_transport_prepare_payload(const struct fsssd_request *req,
				struct fsssd_transport_payload *payload)
{
	size_t name_len;

	memset(payload, 0, sizeof(*payload));

	switch (req->opcode) {
	case fsssd_cmd_nfs_lookup:
	case fsssd_cmd_nfs_create:
	case fsssd_cmd_nfs_mkdir:
	case fsssd_cmd_nfs_remove:
	case fsssd_cmd_nfs_rmdir:
		if (req->name == NULL) {
			return -EINVAL;
		}
		name_len = strlen(req->name);
		if (name_len > FSSSD_NFS_MAX_NAME_LEN) {
			return -ENAMETOOLONG;
		}
		payload->buf = spdk_dma_zmalloc(FSSSD_NFS_MAX_NAME_LEN, 0, NULL);
		if (payload->buf == NULL) {
			return -ENOMEM;
		}
		memcpy(payload->buf, req->name, name_len);
		payload->len = FSSSD_NFS_MAX_NAME_LEN;
		payload->dma_allocated = true;
		return 0;
	case fsssd_cmd_nfs_getattr:
	case fsssd_cmd_nfs_fsstat:
	case fsssd_cmd_nfs_readdir:
		if (req->payload == NULL || req->payload_len == 0) {
			return -EINVAL;
		}
		payload->buf = spdk_dma_zmalloc(req->payload_len, 0, NULL);
		if (payload->buf == NULL) {
			return -ENOMEM;
		}
		payload->len = req->payload_len;
		payload->copy_dst = req->payload;
		payload->copy_out = true;
		payload->dma_allocated = true;
		return 0;
	case fsssd_cmd_nfs_read:
	case fsssd_cmd_nfs_write:
		if (req->iovcnt != 1 || req->iov == NULL || req->iov[0].iov_base == NULL) {
			return -EINVAL;
		}
		if (req->offset % FSSSD_NFS_PAGE_SIZE != 0 || req->size % FSSSD_NFS_PAGE_SIZE != 0) {
			return -EINVAL;
		}
		if (req->size > UINT32_MAX) {
			return -EINVAL;
		}
		payload->buf = req->iov[0].iov_base;
		payload->len = req->size;
		return 0;
	default:
		return 0;
	}
}

static void
fsssd_transport_release_payload(struct fsssd_transport_payload *payload, bool success)
{
	if (success && payload->copy_out) {
		memcpy(payload->copy_dst, payload->buf, payload->len);
	}
	if (payload->dma_allocated) {
		spdk_dma_free(payload->buf);
	}
}

static void
fsssd_transport_build_cmd(struct fsssd_transport *transport, const struct fsssd_request *req,
			  struct spdk_nvme_cmd *cmd)
{
	struct fsssd_cdw3 cdw3 = {};

	memset(cmd, 0, sizeof(*cmd));
	cdw3.s.opcode = req->opcode;
	cmd->opc = SPDK_NVME_OPC_FLUSH;
	cmd->nsid = transport->nsid;
	cmd->rsvd2 = req->handle;
	cmd->rsvd3 = cdw3.val;

	switch (req->opcode) {
	case fsssd_cmd_nfs_lookup:
	case fsssd_cmd_nfs_create:
	case fsssd_cmd_nfs_mkdir:
	case fsssd_cmd_nfs_remove:
	case fsssd_cmd_nfs_rmdir:
		cmd->opc = SPDK_NVME_OPC_WRITE;
		cdw3.s.namelen = req->name == NULL ? 0 : strlen(req->name);
		cmd->rsvd3 = cdw3.val;
		break;
	case fsssd_cmd_nfs_read:
		cmd->opc = SPDK_NVME_OPC_READ;
		cdw3.s.mode = req->size / FSSSD_NFS_PAGE_SIZE;
		cmd->rsvd3 = cdw3.val;
		cmd->cdw13 = req->offset / FSSSD_NFS_PAGE_SIZE;
		break;
	case fsssd_cmd_nfs_write:
		cmd->opc = SPDK_NVME_OPC_WRITE;
		cdw3.s.mode = req->size / FSSSD_NFS_PAGE_SIZE;
		cmd->rsvd3 = cdw3.val;
		cmd->cdw13 = req->offset / FSSSD_NFS_PAGE_SIZE;
		break;
	case fsssd_cmd_nfs_getattr:
	case fsssd_cmd_nfs_fsstat:
	case fsssd_cmd_nfs_readdir:
		cmd->opc = SPDK_NVME_OPC_READ;
		break;
	default:
		break;
	}
}

int
fsssd_transport_submit(struct fsssd_transport *transport, const struct fsssd_request *req,
		       struct fsssd_response *rsp)
{
	struct fsssd_nvme_completion completion = {};
	struct fsssd_transport_payload payload = {};
	struct spdk_nvme_cmd cmd = {};
	int rc;

	if (transport == NULL || req == NULL || rsp == NULL) {
		return -EINVAL;
	}

	memset(rsp, 0, sizeof(*rsp));
	rc = fsssd_transport_prepare_payload(req, &payload);
	if (rc != 0) {
		return rc;
	}

	fsssd_transport_build_cmd(transport, req, &cmd);
	rc = spdk_nvme_ctrlr_cmd_io_raw(transport->ctrlr, transport->qpair, &cmd, payload.buf,
					payload.len, fsssd_transport_nvme_complete, &completion);
	if (rc != 0) {
		fsssd_transport_release_payload(&payload, false);
		return rc;
	}

	rc = fsssd_transport_wait_completion(transport, &completion);
	if (rc != 0) {
		fsssd_transport_release_payload(&payload, false);
		return rc;
	}
	fsssd_transport_release_payload(&payload, true);

	rsp->result = completion.cpl.cdw0 | ((uint64_t)completion.cpl.cdw1 << 32);
	rsp->data_size = completion.cpl.cdw0;

	return 0;
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
