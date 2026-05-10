#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "fsdev_fsssd.h"

struct rpc_fsssd_create {
	char *name;
	struct spdk_fsdev_fsssd_opts opts;
};

static void
free_rpc_fsssd_create(struct rpc_fsssd_create *req)
{
	free(req->name);
	free(req->opts.device);
}

static const struct spdk_json_object_decoder rpc_fsdev_fsssd_create_decoders[] = {
	{"name", offsetof(struct rpc_fsssd_create, name), spdk_json_decode_string},
	{"device", offsetof(struct rpc_fsssd_create, opts.device), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_fsssd_create, opts.nsid), spdk_json_decode_uint32, true},
	{"max_write", offsetof(struct rpc_fsssd_create, opts.max_write), spdk_json_decode_uint32, true},
	{"enable_writeback_cache", offsetof(struct rpc_fsssd_create, opts.writeback_cache_enabled),
	 spdk_json_decode_bool, true},
};

static void
rpc_fsdev_fsssd_create(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_fsssd_create req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_fsdev *fsdev;
	int rc;

	spdk_fsdev_fsssd_get_default_opts(&req.opts);

	if (spdk_json_decode_object(params, rpc_fsdev_fsssd_create_decoders,
				    SPDK_COUNTOF(rpc_fsdev_fsssd_create_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		free_rpc_fsssd_create(&req);
		return;
	}

	rc = spdk_fsdev_fsssd_create(&fsdev, req.name, &req.opts);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to create fsssd %s: rc %d\n", req.name, rc);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		free_rpc_fsssd_create(&req);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, fsdev->name);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_fsssd_create(&req);
}
SPDK_RPC_REGISTER("fsdev_fsssd_create", rpc_fsdev_fsssd_create, SPDK_RPC_RUNTIME)

struct rpc_fsssd_delete {
	char *name;
};

static const struct spdk_json_object_decoder rpc_fsdev_fsssd_delete_decoders[] = {
	{"name", offsetof(struct rpc_fsssd_delete, name), spdk_json_decode_string},
};

static void
rpc_fsssd_delete_cb(void *cb_arg, int fsdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (fsdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, fsdeverrno, spdk_strerror(-fsdeverrno));
	}
}

static void
rpc_fsdev_fsssd_delete(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_fsssd_delete req = {};

	if (spdk_json_decode_object(params, rpc_fsdev_fsssd_delete_decoders,
				    SPDK_COUNTOF(rpc_fsdev_fsssd_delete_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		free(req.name);
		return;
	}

	spdk_fsdev_fsssd_delete(req.name, rpc_fsssd_delete_cb, request);
	free(req.name);
}
SPDK_RPC_REGISTER("fsdev_fsssd_delete", rpc_fsdev_fsssd_delete, SPDK_RPC_RUNTIME)
