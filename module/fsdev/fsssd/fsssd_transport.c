#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/string.h"

#include "fsssd_transport.h"

#define FSSSD_PRP_LIST_ENTRIES (FSSSD_NFS_PAGE_SIZE / sizeof(uint64_t))
#define FSSSD_PRP_MAX_ENTRIES (FSSSD_PRP_LIST_ENTRIES + 1)
#define FSSSD_NFS_READ_COUNT_MASK 0x7fffffffU
#define FSSSD_ADMIN_QUEUE_SIZE 32
#define FSSSD_IO_QUEUE_SIZE 256

struct fsssd_transport {
	char *name;
	char *device;
	uint32_t nsid;
	struct spdk_nvme_transport_id trid;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;
};

struct fsssd_transport_channel {
	struct fsssd_transport *transport;
	struct spdk_nvme_qpair *qpair;
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

static int
fsssd_transport_parse_hostnqn(char *hostnqn, size_t hostnqn_size, const char *device)
{
	const char *value;
	size_t len;

	value = strcasestr(device, "hostnqn:");
	if (value == NULL) {
		value = strcasestr(device, "hostnqn=");
	}
	if (value == NULL) {
		return 0;
	}

	value += strlen("hostnqn:");
	len = strcspn(value, " \t\n");
	if (len >= hostnqn_size) {
		SPDK_ERRLOG("hostnqn length %zu exceeds maximum allowed %zu\n", len, hostnqn_size - 1);
		return -EINVAL;
	}

	memcpy(hostnqn, value, len);
	hostnqn[len] = '\0';
	return 0;
}

struct fsssd_transport *
fsssd_transport_create(const struct fsssd_transport_opts *opts)
{
	struct spdk_nvme_ctrlr_opts ctrlr_opts;
	struct fsssd_transport *transport;
	int rc;

	if (opts == NULL || opts->name == NULL || opts->device == NULL) {
		SPDK_ERRLOG("Invalid fsssd transport options\n");
		return NULL;
	}

	transport = calloc(1, sizeof(*transport));
	if (transport == NULL) {
		SPDK_ERRLOG("Failed to allocate fsssd transport\n");
		return NULL;
	}

	transport->name = strdup(opts->name);
	transport->device = strdup(opts->device);
	transport->nsid = opts->nsid;
	if (transport->name == NULL || transport->device == NULL) {
		SPDK_ERRLOG("Failed to duplicate fsssd transport strings\n");
		fsssd_transport_destroy(transport);
		return NULL;
	}

	rc = fsssd_transport_parse_trid(&transport->trid, transport->device);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to parse fsssd transport ID: %s\n", transport->device);
		fsssd_transport_destroy(transport);
		return NULL;
	}

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctrlr_opts, sizeof(ctrlr_opts));
	ctrlr_opts.admin_queue_size = FSSSD_ADMIN_QUEUE_SIZE;
	ctrlr_opts.io_queue_size = FSSSD_IO_QUEUE_SIZE;
	ctrlr_opts.io_queue_requests = FSSSD_IO_QUEUE_SIZE;
	ctrlr_opts.keep_alive_timeout_ms = 0;
	rc = fsssd_transport_parse_hostnqn(ctrlr_opts.hostnqn, sizeof(ctrlr_opts.hostnqn),
					   transport->device);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to parse fsssd hostnqn from transport ID: %s\n", transport->device);
		fsssd_transport_destroy(transport);
		return NULL;
	}

	transport->ctrlr = spdk_nvme_connect(&transport->trid, &ctrlr_opts, sizeof(ctrlr_opts));
	if (transport->ctrlr == NULL) {
		SPDK_ERRLOG("Failed to connect NVMe controller for fsssd device: %s\n", transport->device);
		fsssd_transport_destroy(transport);
		return NULL;
	}

	if (transport->nsid == 0) {
		transport->nsid = spdk_nvme_ctrlr_get_first_active_ns(transport->ctrlr);
		if (transport->nsid == 0) {
			SPDK_ERRLOG("No active NVMe namespace found for fsssd device: %s\n",
				    transport->device);
			fsssd_transport_destroy(transport);
			return NULL;
		}
		SPDK_NOTICELOG("Using first active NVMe namespace %" PRIu32 " for fsssd device: %s\n",
			       transport->nsid, transport->device);
	}

	transport->ns = spdk_nvme_ctrlr_get_ns(transport->ctrlr, transport->nsid);
	if (transport->ns == NULL || !spdk_nvme_ns_is_active(transport->ns)) {
		SPDK_ERRLOG("NVMe namespace %" PRIu32 " is not active for fsssd device: %s\n",
			    transport->nsid, transport->device);
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

	if (transport->ctrlr != NULL) {
		spdk_nvme_detach(transport->ctrlr);
	}
	free(transport->name);
	free(transport->device);
	free(transport);
}

struct fsssd_transport_channel *
fsssd_transport_channel_create(struct fsssd_transport *transport)
{
	struct fsssd_transport_channel *channel;
	struct spdk_nvme_io_qpair_opts qpair_opts;

	if (transport == NULL) {
		return NULL;
	}

	channel = calloc(1, sizeof(*channel));
	if (channel == NULL) {
		return NULL;
	}

	channel->transport = transport;
	spdk_nvme_ctrlr_get_default_io_qpair_opts(transport->ctrlr, &qpair_opts,
			sizeof(qpair_opts));
	if (transport->trid.trtype == SPDK_NVME_TRANSPORT_RDMA) {
		qpair_opts.delay_cmd_submit = true;
	}
	channel->qpair = spdk_nvme_ctrlr_alloc_io_qpair(transport->ctrlr, &qpair_opts,
			 sizeof(qpair_opts));
	if (channel->qpair == NULL) {
		free(channel);
		return NULL;
	}

	return channel;
}

void
fsssd_transport_channel_destroy(struct fsssd_transport_channel *channel)
{
	if (channel == NULL) {
		return;
	}

	if (channel->qpair != NULL) {
		spdk_nvme_ctrlr_free_io_qpair(channel->qpair);
	}
	free(channel);
}

int
fsssd_transport_channel_poll(struct fsssd_transport_channel *channel)
{
	int32_t admin_rc = 0;
	int32_t io_rc = 0;

	if (channel == NULL || channel->qpair == NULL) {
		return -EINVAL;
	}

	// admin_rc = spdk_nvme_ctrlr_process_admin_completions(channel->transport->ctrlr);
	// if (admin_rc < 0) {
	// 	return admin_rc;
	// }

	io_rc = spdk_nvme_qpair_process_completions(channel->qpair, 0);
	if (io_rc < 0) {
		return io_rc;
	}

	return admin_rc + io_rc;
}

static uint32_t
fsssd_transport_data_size(enum fsssd_nfs_opcode opcode, uint32_t cdw0)
{
	if (opcode == fsssd_cmd_nfs_read) {
		return cdw0 & FSSSD_NFS_READ_COUNT_MASK;
	}

	return cdw0;
}

static int
fsssd_transport_status_to_errno(const struct spdk_nvme_cpl *cpl)
{
	if (!spdk_nvme_cpl_is_error(cpl)) {
		return 0;
	}

	if (cpl->status.sct == SPDK_NVME_SCT_GENERIC) {
		switch (cpl->status.sc) {
		case ENOENT:
			return -ENOENT;
		case EEXIST:
			return -EEXIST;
		case ENOTEMPTY:
			return -ENOTEMPTY;
		case ESTALE:
			return -ESTALE;
		case EACCES:
			return -EACCES;
		case ENOMEM:
			return -ENOMEM;
		case EINVAL:
			return -EINVAL;
		case ENOSPC:
			return -ENOSPC;
		default:
			break;
		}
	}

	return -EIO;
}

static int
fsssd_transport_iov_len(const struct iovec *iov, uint32_t iovcnt, size_t *len)
{
	size_t total = 0;
	uint32_t i;

	if (iov == NULL || iovcnt == 0 || len == NULL) {
		return -EINVAL;
	}

	for (i = 0; i < iovcnt; i++) {
		if (iov[i].iov_base == NULL) {
			return -EINVAL;
		}
		if (SIZE_MAX - total < iov[i].iov_len) {
			return -EINVAL;
		}
		total += iov[i].iov_len;
	}

	*len = total;
	return 0;
}

static void
fsssd_transport_iov_copy_to_buf(void *buf, const struct iovec *iov, uint32_t iovcnt)
{
	uint8_t *dst = buf;
	uint32_t i;

	for (i = 0; i < iovcnt; i++) {
		memcpy(dst, iov[i].iov_base, iov[i].iov_len);
		dst += iov[i].iov_len;
	}
}

static void
fsssd_transport_iov_copy_from_buf(struct iovec *iov, uint32_t iovcnt, const void *buf)
{
	const uint8_t *src = buf;
	uint32_t i;

	for (i = 0; i < iovcnt; i++) {
		memcpy(iov[i].iov_base, src, iov[i].iov_len);
		src += iov[i].iov_len;
	}
}

static int
fsssd_transport_prepare_payload(const struct fsssd_request *req,
				struct fsssd_transport_payload *payload)
{
	size_t name_len;
	size_t iov_len;

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
		if (fsssd_transport_iov_len(req->iov, req->iovcnt, &iov_len) != 0) {
			return -EINVAL;
		}
		if (req->offset % FSSSD_NFS_PAGE_SIZE != 0 || req->size % FSSSD_NFS_PAGE_SIZE != 0) {
			return -EINVAL;
		}
		if (req->size > UINT32_MAX || iov_len != req->size) {
			return -EINVAL;
		}
		payload->len = req->size;
		payload->iov = req->iov;
		payload->iovcnt = req->iovcnt;
		if (req->iovcnt == 1) {
			payload->buf = req->iov[0].iov_base;
		}
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
	if (success && payload->iov_copy_out) {
		fsssd_transport_iov_copy_from_buf(payload->copy_iov, payload->copy_iovcnt, payload->buf);
	}
	if (payload->dma_allocated) {
		spdk_dma_free(payload->buf);
	}
	if (payload->prp_list != NULL) {
		spdk_dma_free(payload->prp_list);
	}
}

static int
fsssd_transport_use_bounce(const struct fsssd_request *req, struct fsssd_transport_payload *payload)
{
	payload->buf = spdk_dma_zmalloc(payload->len, FSSSD_NFS_PAGE_SIZE, NULL);
	if (payload->buf == NULL) {
		return -ENOMEM;
	}

	payload->dma_allocated = true;
	payload->copy_iov = payload->iov;
	payload->copy_iovcnt = payload->iovcnt;
	if (req->opcode == fsssd_cmd_nfs_write) {
		fsssd_transport_iov_copy_to_buf(payload->buf, payload->iov, payload->iovcnt);
	} else {
		payload->iov_copy_out = true;
	}
	payload->iov = NULL;
	payload->iovcnt = 0;

	return 0;
}

static int
fsssd_transport_fill_prps(const struct iovec *iov, uint32_t iovcnt, uint64_t *prps,
			  uint32_t *nr_prps, int fallback_rc)
{
	uint64_t phys_len;
	uint64_t phys_addr;
	uint8_t *vaddr;
	size_t remaining;
	uint32_t page_remaining;
	uint32_t xfer_len;
	uint64_t expected_phys = 0;
	uint32_t i;

	*nr_prps = 0;
	page_remaining = 0;
	for (i = 0; i < iovcnt; i++) {
		vaddr = iov[i].iov_base;
		remaining = iov[i].iov_len;
		while (remaining > 0) {
			phys_len = remaining;
			phys_addr = spdk_vtophys(vaddr, &phys_len);
			if (phys_addr == SPDK_VTOPHYS_ERROR || phys_len == 0) {
				return fallback_rc;
			}

			while (phys_len > 0) {
				if (page_remaining == 0) {
					if (*nr_prps == 0 &&
					    (phys_addr & (FSSSD_NFS_PAGE_SIZE - 1)) != 0 &&
					    fallback_rc == -EAGAIN) {
						return fallback_rc;
					}
					if (*nr_prps > 0 &&
					    (phys_addr & (FSSSD_NFS_PAGE_SIZE - 1)) != 0) {
						return fallback_rc;
					}
					if (*nr_prps == FSSSD_PRP_MAX_ENTRIES) {
						return -EINVAL;
					}
					prps[(*nr_prps)++] = phys_addr;
					page_remaining = FSSSD_NFS_PAGE_SIZE -
							 (phys_addr & (FSSSD_NFS_PAGE_SIZE - 1));
					expected_phys = phys_addr;
				}

				if (phys_addr != expected_phys) {
					return fallback_rc;
				}

				xfer_len = spdk_min((uint64_t)page_remaining, phys_len);
				vaddr += xfer_len;
				remaining -= xfer_len;
				phys_addr += xfer_len;
				phys_len -= xfer_len;
				expected_phys += xfer_len;
				page_remaining -= xfer_len;
			}
		}
	}

	return 0;
}

static int
fsssd_transport_build_prps(struct fsssd_transport_payload *payload, struct spdk_nvme_cmd *cmd)
{
	struct iovec iov;
	const struct iovec *prp_iov;
	uint64_t prps[FSSSD_PRP_MAX_ENTRIES];
	uint64_t phys_len;
	uint64_t prp_list_phys;
	uint32_t nr_prps = 0;
	int fallback_rc;
	int rc;

	if (payload->len == 0) {
		return 0;
	}

	if (payload->iov != NULL) {
		prp_iov = payload->iov;
		fallback_rc = -EAGAIN;
	} else {
		iov.iov_base = payload->buf;
		iov.iov_len = payload->len;
		prp_iov = &iov;
		fallback_rc = -EFAULT;
	}

	rc = fsssd_transport_fill_prps(prp_iov, payload->iov != NULL ? payload->iovcnt : 1,
				       prps, &nr_prps, fallback_rc);
	if (rc != 0) {
		return rc;
	}
	if (nr_prps == 0) {
		return -EINVAL;
	}

	cmd->psdt = SPDK_NVME_PSDT_PRP;
	cmd->dptr.prp.prp1 = prps[0];
	if (nr_prps == 1) {
		cmd->dptr.prp.prp2 = 0;
	} else if (nr_prps == 2) {
		cmd->dptr.prp.prp2 = prps[1];
	} else {
		payload->prp_list = spdk_dma_zmalloc(FSSSD_NFS_PAGE_SIZE, FSSSD_NFS_PAGE_SIZE, NULL);
		if (payload->prp_list == NULL) {
			return -ENOMEM;
		}
		memcpy(payload->prp_list, &prps[1], (nr_prps - 1) * sizeof(uint64_t));
		phys_len = FSSSD_NFS_PAGE_SIZE;
		prp_list_phys = spdk_vtophys(payload->prp_list, &phys_len);
		if (prp_list_phys == SPDK_VTOPHYS_ERROR || phys_len < FSSSD_NFS_PAGE_SIZE) {
			return -EFAULT;
		}
		cmd->dptr.prp.prp2 = prp_list_phys;
	}

	return 0;
}

static void
fsssd_transport_pack_name(struct spdk_nvme_cmd *cmd, const char *name, size_t name_len)
{
	uint8_t inline_name[FSSSD_NFS_MAX_NAME_LEN] = {};

	memcpy(inline_name, name, name_len);
	memcpy(&cmd->cdw10, inline_name, sizeof(cmd->cdw10));
	memcpy(&cmd->cdw11, inline_name + 4, sizeof(cmd->cdw11));
	memcpy(&cmd->cdw12, inline_name + 8, sizeof(cmd->cdw12));
	memcpy(&cmd->cdw13, inline_name + 12, sizeof(cmd->cdw13));
	memcpy(&cmd->cdw14, inline_name + 16, sizeof(cmd->cdw14));
	memcpy(&cmd->cdw15, inline_name + 20, sizeof(cmd->cdw15));
}

static void
fsssd_transport_build_cmd(struct fsssd_transport *transport, const struct fsssd_request *req,
			  struct spdk_nvme_cmd *cmd)
{
	struct fsssd_cdw3 cdw3 = {};
	size_t name_len;

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
		name_len = req->name == NULL ? 0 : strlen(req->name);
		cdw3.s.namelen = name_len;
		cdw3.s.mode = req->mode;
		cmd->rsvd3 = cdw3.val;
		fsssd_transport_pack_name(cmd, req->name, name_len);
		break;
	case fsssd_cmd_nfs_read:
		cmd->opc = SPDK_NVME_OPC_READ;
		cdw3.s.namelen = req->size / FSSSD_NFS_PAGE_SIZE;
		cmd->rsvd3 = cdw3.val;
		cmd->cdw13 = req->offset / FSSSD_NFS_PAGE_SIZE;
		break;
	case fsssd_cmd_nfs_write:
		cmd->opc = SPDK_NVME_OPC_WRITE;
		cdw3.s.namelen = req->size / FSSSD_NFS_PAGE_SIZE;
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

static int
fsssd_transport_submit_nvme(struct fsssd_transport *transport, struct spdk_nvme_qpair *qpair,
			    const struct fsssd_request *req,
			    struct fsssd_transport_payload *payload,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_cmd cmd = {};
	int rc;

	fsssd_transport_build_cmd(transport, req, &cmd);
	if (payload->len != 0 && transport->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {  /* TODO: it may not support NVMe-oF */
		rc = fsssd_transport_build_prps(payload, &cmd);
		if (rc == -EAGAIN) {
			// SPDK_NOTICELOG("fsssd prp path: fallback bounce opcode=%u len=%u iovcnt=%u\n",
			// 	       req->opcode, payload->len, payload->iovcnt);
			rc = fsssd_transport_use_bounce(req, payload);
			if (rc == 0) {
				rc = fsssd_transport_build_prps(payload, &cmd);
				if (rc == 0) {
					// SPDK_NOTICELOG("fsssd prp path: bounce prp opcode=%u len=%u\n",
					// 	       req->opcode, payload->len);
				}
			}
		} else if (rc == 0) {
			// SPDK_NOTICELOG("fsssd prp path: zero-copy opcode=%u len=%u iovcnt=%u\n",
			// 	       req->opcode, payload->len, payload->iovcnt);
		}
		if (rc != 0) {
			return rc;
		}
		return spdk_nvme_ctrlr_io_cmd_raw_no_payload_build(transport->ctrlr, qpair,
				&cmd, cb_fn, cb_arg);
	}

	if (payload->len != 0 && payload->buf == NULL) {
		rc = fsssd_transport_use_bounce(req, payload);
		if (rc != 0) {
			return rc;
		}
	}

	return spdk_nvme_ctrlr_cmd_io_raw(transport->ctrlr, qpair, &cmd, payload->buf,
					  payload->len, cb_fn, cb_arg);
}

static void
fsssd_transport_async_complete(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct fsssd_transport_async_request *async_req = ctx;
	struct fsssd_response rsp = {};
	int status;

	status = fsssd_transport_status_to_errno(cpl);
	if (status == 0) {
		async_req->rsp.result = cpl->cdw0 | ((uint64_t)cpl->cdw1 << 32);
		async_req->rsp.data_size = fsssd_transport_data_size(async_req->opcode, cpl->cdw0);
	}
	rsp = async_req->rsp;

	fsssd_transport_release_payload(&async_req->payload, status == 0);
	async_req->cb_fn(async_req->cb_arg, status, &rsp);
}

int
fsssd_transport_submit_async(struct fsssd_transport *transport,
			     struct fsssd_transport_channel *channel,
			     const struct fsssd_request *req,
			     struct fsssd_transport_async_request *async_req,
			     fsssd_transport_complete_cb cb_fn, void *cb_arg)
{
	int rc;

	if (transport == NULL || channel == NULL || channel->qpair == NULL ||
	    req == NULL || async_req == NULL || cb_fn == NULL) {
		return -EINVAL;
	}

	memset(async_req, 0, sizeof(*async_req));
	rc = fsssd_transport_prepare_payload(req, &async_req->payload);
	if (rc != 0) {
		return rc;
	}

	async_req->cb_fn = cb_fn;
	async_req->cb_arg = cb_arg;
	async_req->opcode = req->opcode;
	rc = fsssd_transport_submit_nvme(transport, channel->qpair, req, &async_req->payload,
					 fsssd_transport_async_complete, async_req);
	if (rc != 0) {
		fsssd_transport_release_payload(&async_req->payload, false);
		return rc;
	}

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
