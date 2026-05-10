#ifndef FSSSD_PROTO_H
#define FSSSD_PROTO_H

#include "spdk/stdinc.h"

enum fsssd_nfs_opcode {
	fsssd_cmd_nfs_symlink	= 0x50,
	fsssd_cmd_nfs_write	= 0x51,
	fsssd_cmd_nfs_read	= 0x52,
	fsssd_cmd_nfs_lookup	= 0x54,
	fsssd_cmd_nfs_mkdir	= 0x55,
	fsssd_cmd_nfs_readdir	= 0x56,
	fsssd_cmd_nfs_access	= 0x58,
	fsssd_cmd_nfs_commit	= 0x59,
	fsssd_cmd_nfs_setattr	= 0x5D,
	fsssd_cmd_nfs_getattr	= 0x5E,
	fsssd_cmd_nfs_create	= 0x60,
	fsssd_cmd_nfs_rename	= 0x61,
	fsssd_cmd_nfs_fsstat	= 0x62,
	fsssd_cmd_nfs_remove	= 0x64,
	fsssd_cmd_nfs_rmdir	= 0x65,
	fsssd_cmd_nfs_readlink	= 0x66,
	fsssd_cmd_nfs_mnt	= 0x68,
	fsssd_cmd_nfs_fsinfo	= 0x6A,
	fsssd_cmd_nfs_umnt	= 0x6C,
	fsssd_cmd_nfs_pathconf	= 0x6E,
};

struct fsssd_cdw3 {
	union {
		struct {
			uint8_t opcode;
			uint8_t namelen;
			uint16_t mode;
		} s;
		uint32_t val;
	};
};

#endif
