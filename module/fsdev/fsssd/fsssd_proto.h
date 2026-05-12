#ifndef FSSSD_PROTO_H
#define FSSSD_PROTO_H

#include "spdk/stdinc.h"

#define FSSSD_NFS_PAGE_SIZE 4096
#define FSSSD_NFS_MAX_NAME_LEN 24
#define FSSSD_NVME_CMD_TIMEOUT_SEC 30

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

struct fsssd_nfs_fsid {
	uint64_t major;
	uint64_t minor;
};

struct fsssd_nfs_timespec {
	int64_t tv_sec;
	int64_t tv_nsec;
};

struct fsssd_nfs_fattr {
	uint16_t valid1;
	uint16_t valid2;
	uint16_t mode;
	uint16_t reserved0;
	uint32_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint32_t rdev;
	uint64_t size;
	uint64_t used;
	struct fsssd_nfs_fsid fsid;
	uint64_t fileid;
	uint64_t reserved;
	struct fsssd_nfs_timespec atime;
	struct fsssd_nfs_timespec mtime;
	struct fsssd_nfs_timespec ctime;
	uint64_t change_attr;
	uint64_t pre_change_attr;
	uint64_t pre_size;
	struct fsssd_nfs_timespec pre_mtime;
	struct fsssd_nfs_timespec pre_ctime;
	uint64_t time_start;
	uint64_t gencount;
	uint32_t tenant_id;
	uint32_t dummy1;
	uint64_t dummy2;
	uint64_t dummy3;
	uint64_t dummy4;
};

struct fsssd_nfs_fsstat {
	uint64_t fattr;
	uint64_t tbytes;
	uint64_t fbytes;
	uint64_t abytes;
	uint64_t tfiles;
	uint64_t ffiles;
	uint64_t afiles;
};

#endif
