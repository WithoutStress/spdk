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

/*
 * The device keeps the CQE status for transport errors only. FS-level errno
 * arrives in cdw0 as a negative value, except for read/write (cdw0 is the
 * byte count, bit 31 of read is EOF) and readdir (own result format).
 */
static int
fsssd_transport_cpl_to_errno(enum fsssd_nfs_opcode opcode, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		return -EIO;
	}

	switch (opcode) {
	case fsssd_cmd_nfs_read:
	case fsssd_cmd_nfs_write:
	case fsssd_cmd_nfs_readdir:
		return 0;
	default:
		break;
	}

	if ((int32_t)cpl->cdw0 < 0) {
		return (int32_t)cpl->cdw0;
	}

	return 0;
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

/* Copy between buf and the byte window [off, off + len) of iov */
static void
fsssd_transport_iov_copy(const struct iovec *iov, uint32_t iovcnt, size_t off, size_t len,
			 void *buf, bool to_buf)
{
	uint8_t *p = buf;
	uint8_t *base;
	size_t n;
	uint32_t i;

	for (i = 0; i < iovcnt && len > 0; i++) {
		if (off >= iov[i].iov_len) {
			off -= iov[i].iov_len;
			continue;
		}
		base = (uint8_t *)iov[i].iov_base + off;
		n = spdk_min(iov[i].iov_len - off, len);
		if (to_buf) {
			memcpy(p, base, n);
		} else {
			memcpy(base, p, n);
		}
		p += n;
		len -= n;
		off = 0;
	}
}

/* one zeroed page: request bytes at REQ_OFF, device replies fattr at OBJ/DIR_OFF */
static int
fsssd_transport_prepare_meta_page(const void *data, size_t len, struct fsssd_nfs_fattr *attr_dst,
				  struct fsssd_transport_payload *payload)
{
	payload->buf = spdk_dma_zmalloc(FSSSD_NFS_PAGE_SIZE, FSSSD_NFS_PAGE_SIZE, NULL);
	if (payload->buf == NULL) {
		return -ENOMEM;
	}

	memcpy((uint8_t *)payload->buf + FSSSD_NFS_META_REQ_OFF, data, len);
	payload->len = FSSSD_NFS_PAGE_SIZE;
	payload->attr_dst = attr_dst;
	payload->dma_allocated = true;
	return 0;
}

static int
fsssd_transport_prepare_payload(const struct fsssd_transport *transport,
				const struct fsssd_request *req,
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
		if (name_len == 0) {
			return -EINVAL;
		}
		if (name_len > FSSSD_NFS_MAX_NAME_LEN) {
			return -ENAMETOOLONG;
		}
		/* NVMe-oF cannot deliver the request page: only inline names reach the device */
		if (name_len > FSSSD_NFS_NAME_INLINE_MAX &&
		    transport->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
			return -ENAMETOOLONG;
		}
		return fsssd_transport_prepare_meta_page(req->name, name_len, req->obj_attr, payload);
	case fsssd_cmd_nfs_setattr:
		if (req->payload == NULL || req->payload_len > FSSSD_NFS_META_OBJ_OFF) {
			return -EINVAL;
		}
		return fsssd_transport_prepare_meta_page(req->payload, req->payload_len, req->obj_attr,
				payload);
	case fsssd_cmd_nfs_getattr:
	case fsssd_cmd_nfs_fsstat:
	case fsssd_cmd_nfs_readdir:
		if (req->payload == NULL || req->payload_len == 0) {
			return -EINVAL;
		}
		/* readdir: the device fills (CDW12 + 1) whole pages */
		if (req->opcode == fsssd_cmd_nfs_readdir &&
		    req->payload_len % FSSSD_NFS_PAGE_SIZE != 0) {
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
	if (success && payload->attr_dst != NULL) {
		memcpy(payload->attr_dst, (uint8_t *)payload->buf + FSSSD_NFS_META_OBJ_OFF,
		       sizeof(*payload->attr_dst));
	}
	if (success && payload->iov_copy_out) {
		fsssd_transport_iov_copy(payload->copy_iov, payload->copy_iovcnt, payload->copy_iov_off,
					 payload->len, payload->buf, false);
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
	payload->copy_iov_off = payload->iov_off;
	if (req->opcode == fsssd_cmd_nfs_write) {
		fsssd_transport_iov_copy(payload->iov, payload->iovcnt, payload->iov_off, payload->len,
					 payload->buf, true);
	} else {
		payload->iov_copy_out = true;
	}
	payload->iov = NULL;
	payload->iovcnt = 0;
	payload->iov_off = 0;

	return 0;
}

static int
fsssd_transport_fill_prps(const struct iovec *iov, uint32_t iovcnt, size_t off, size_t len,
			  uint64_t *prps, uint32_t *nr_prps, int fallback_rc)
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
	for (i = 0; i < iovcnt && len > 0; i++) {
		if (off >= iov[i].iov_len) {
			off -= iov[i].iov_len;
			continue;
		}
		vaddr = (uint8_t *)iov[i].iov_base + off;
		remaining = spdk_min(iov[i].iov_len - off, len);
		len -= remaining;
		off = 0;
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
	uint64_t prps[FSSSD_PRP_MAX_ENTRIES];
	uint64_t phys_len;
	uint64_t prp_list_phys;
	uint32_t nr_prps = 0;
	int rc;

	if (payload->len == 0) {
		return 0;
	}

	if (payload->iov != NULL) {
		rc = fsssd_transport_fill_prps(payload->iov, payload->iovcnt, payload->iov_off,
					       payload->len, prps, &nr_prps, -EAGAIN);
	} else {
		iov.iov_base = payload->buf;
		iov.iov_len = payload->len;
		rc = fsssd_transport_fill_prps(&iov, 1, 0, payload->len, prps, &nr_prps, -EFAULT);
	}
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
fsssd_transport_build_cmd(struct fsssd_transport *transport, const struct fsssd_request *req,
			  const struct fsssd_transport_payload *payload, struct spdk_nvme_cmd *cmd)
{
	struct fsssd_cdw2 cdw2 = {};
	uint32_t npages = 1;
	uint64_t slba = 0;
	uint32_t w[3] = {};
	size_t name_len;

	memset(cmd, 0, sizeof(*cmd));
	cdw2.s.opcode = req->opcode;
	cmd->opc = SPDK_NVME_OPC_READ;
	cmd->nsid = transport->nsid;

	switch (req->opcode) {
	case fsssd_cmd_nfs_mnt:
		/* mount keeps the legacy layout: fh 0 in CDW2, opcode word in CDW3 */
		cmd->opc = SPDK_NVME_OPC_FLUSH;
		cmd->rsvd3 = cdw2.val;
		return;
	case fsssd_cmd_nfs_lookup:
	case fsssd_cmd_nfs_create:
	case fsssd_cmd_nfs_mkdir:
	case fsssd_cmd_nfs_remove:
	case fsssd_cmd_nfs_rmdir:
		/* NVMe READ opcode tells the device a reply page is attached */
		name_len = strlen(req->name);
		cdw2.s.namelen = name_len;
		cdw2.s.mode = req->mode;
		if (name_len <= FSSSD_NFS_NAME_INLINE_MAX) {
			memcpy(w, req->name, name_len);
			cmd->cdw13 = w[0];
			cmd->cdw14 = w[1];
			cmd->cdw15 = w[2];
		}
		break;
	case fsssd_cmd_nfs_setattr:
	case fsssd_cmd_nfs_getattr:
	case fsssd_cmd_nfs_fsstat:
		cdw2.s.namelen = npages;
		break;
	case fsssd_cmd_nfs_readdir:
		npages = payload->len / FSSSD_NFS_PAGE_SIZE;
		cdw2.s.namelen = npages;
		slba = req->offset;	/* cookie */
		break;
	case fsssd_cmd_nfs_read:
	case fsssd_cmd_nfs_write:
		if (req->opcode == fsssd_cmd_nfs_write) {
			cmd->opc = SPDK_NVME_OPC_WRITE;
		}
		npages = req->size / FSSSD_NFS_PAGE_SIZE;
		cdw2.s.namelen = npages;
		cdw2.s.mode = req->offset & (FSSSD_NFS_PAGE_SIZE - 1);
		slba = req->offset / FSSSD_NFS_PAGE_SIZE;
		break;
	default:
		break;
	}

	cmd->rsvd2 = cdw2.val;
	cmd->rsvd3 = req->handle;
	cmd->cdw10 = slba;
	cmd->cdw11 = slba >> 32;
	cmd->cdw12 = npages - 1;
	/* CDW14/15 uid/gid stay 0 (root) unless a name is inline */
}

static int
fsssd_transport_submit_nvme(struct fsssd_transport *transport, struct spdk_nvme_qpair *qpair,
			    const struct fsssd_request *req,
			    struct fsssd_transport_payload *payload,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_cmd cmd = {};
	int rc;

	fsssd_transport_build_cmd(transport, req, payload, &cmd);
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

/*
 * Bytes of a read/write before the node boundary it crosses, 0 if it fits in
 * one node. Like client/nfs, at most one boundary is handled: the smallest
 * node (826 pages) is larger than any PCIe command (PRP list: 513 pages).
 */
static int
fsssd_transport_split_len(const struct fsssd_request *req, uint32_t *first_len)
{
	uint64_t first = req->offset / FSSSD_NFS_PAGE_SIZE;
	uint64_t last = first + req->size / FSSSD_NFS_PAGE_SIZE;	/* exclusive */
	uint64_t end;

	*first_len = 0;
	if (req->opcode != fsssd_cmd_nfs_read && req->opcode != fsssd_cmd_nfs_write) {
		return 0;
	}

	end = fsssd_node_end(first);
	if (last <= end) {
		return 0;
	}
	if (last > fsssd_node_end(end)) {
		return -EINVAL;
	}

	*first_len = (end - first) * FSSSD_NFS_PAGE_SIZE;
	return 0;
}

/* Narrow a read/write payload to the byte window [off, off + len) of its data */
static void
fsssd_transport_payload_slice(const struct fsssd_transport_payload *src, uint32_t off,
			      uint32_t len, struct fsssd_transport_payload *dst)
{
	memset(dst, 0, sizeof(*dst));
	dst->len = len;
	dst->iov = src->iov;
	dst->iovcnt = src->iovcnt;
	dst->iov_off = src->iov_off + off;
	while (dst->iov_off >= dst->iov->iov_len) {
		dst->iov_off -= dst->iov->iov_len;
		dst->iov++;
		dst->iovcnt--;
	}
	if (dst->iov_off + len <= dst->iov->iov_len) {
		dst->buf = (uint8_t *)dst->iov->iov_base + dst->iov_off;
	}
}

/*
 * Merge the halves of a node-boundary split as client/nfs does: either half
 * failing fails the request, and a short or EOF first half drops the second.
 */
static void
fsssd_transport_async_finish(struct fsssd_transport_async_request *async_req)
{
	struct fsssd_transport_sub_request *first = &async_req->sub[0];
	struct fsssd_transport_sub_request *last = &async_req->sub[1];
	bool is_read = async_req->opcode == fsssd_cmd_nfs_read;
	struct fsssd_response rsp = {};
	uint32_t count;
	uint32_t eof;
	int status;

	status = first->status;
	if (status == 0 && async_req->nr_sub == 2) {
		status = last->status;
	}

	if (status == 0 && async_req->nr_sub == 1) {
		rsp.result = first->cdw0 | ((uint64_t)first->cdw1 << 32);
		rsp.data_size = fsssd_transport_data_size(async_req->opcode, first->cdw0);
	} else if (status == 0) {
		count = fsssd_transport_data_size(async_req->opcode, first->cdw0);
		eof = is_read ? first->cdw0 & ~FSSSD_NFS_READ_COUNT_MASK : 0;
		if (eof || count == async_req->first_len) {
			count += fsssd_transport_data_size(async_req->opcode, last->cdw0);
			eof |= is_read ? last->cdw0 & ~FSSSD_NFS_READ_COUNT_MASK : 0;
		}
		rsp.result = count | eof;
		rsp.data_size = count;
	}

	async_req->cb_fn(async_req->cb_arg, status, &rsp);
}

static void
fsssd_transport_sub_complete(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct fsssd_transport_sub_request *sub = ctx;
	struct fsssd_transport_async_request *async_req = sub->parent;

	sub->status = fsssd_transport_cpl_to_errno(async_req->opcode, cpl);
	sub->cdw0 = cpl->cdw0;
	sub->cdw1 = cpl->cdw1;
	fsssd_transport_release_payload(&sub->payload, sub->status == 0);

	if (--async_req->outstanding == 0) {
		fsssd_transport_async_finish(async_req);
	}
}

int
fsssd_transport_submit_async(struct fsssd_transport *transport,
			     struct fsssd_transport_channel *channel,
			     const struct fsssd_request *req,
			     struct fsssd_transport_async_request *async_req,
			     fsssd_transport_complete_cb cb_fn, void *cb_arg)
{
	struct fsssd_transport_payload payload;
	struct fsssd_transport_sub_request *sub;
	struct fsssd_request half;
	uint32_t first_len;
	uint32_t i;
	int rc;

	if (transport == NULL || channel == NULL || channel->qpair == NULL ||
	    req == NULL || async_req == NULL || cb_fn == NULL) {
		return -EINVAL;
	}

	rc = fsssd_transport_split_len(req, &first_len);
	if (rc != 0) {
		return rc;
	}

	memset(async_req, 0, sizeof(*async_req));
	rc = fsssd_transport_prepare_payload(transport, req, &payload);
	if (rc != 0) {
		return rc;
	}

	async_req->cb_fn = cb_fn;
	async_req->cb_arg = cb_arg;
	async_req->opcode = req->opcode;
	async_req->first_len = first_len;
	async_req->nr_sub = first_len != 0 ? 2 : 1;
	async_req->outstanding = async_req->nr_sub;
	async_req->sub[0].parent = async_req;
	async_req->sub[1].parent = async_req;
	if (first_len == 0) {
		async_req->sub[0].payload = payload;
	} else {
		/* read/write payloads only reference the caller's iov, nothing to free */
		fsssd_transport_payload_slice(&payload, 0, first_len, &async_req->sub[0].payload);
		fsssd_transport_payload_slice(&payload, first_len, payload.len - first_len,
					      &async_req->sub[1].payload);
	}

	for (i = 0; i < async_req->nr_sub; i++) {
		sub = &async_req->sub[i];
		half = *req;
		if (first_len != 0) {
			half.offset = req->offset + (i == 0 ? 0 : first_len);
			half.size = sub->payload.len;
		}
		rc = fsssd_transport_submit_nvme(transport, channel->qpair, &half, &sub->payload,
						 fsssd_transport_sub_complete, sub);
		if (rc != 0) {
			break;
		}
	}

	if (rc == 0) {
		return 0;
	}

	fsssd_transport_release_payload(&async_req->sub[i].payload, false);
	if (i == 0) {
		/* nothing in flight */
		return rc;
	}

	/* second half failed to submit: fail the request once the first completes */
	async_req->sub[i].status = rc;
	if (--async_req->outstanding == 0) {
		fsssd_transport_async_finish(async_req);
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
