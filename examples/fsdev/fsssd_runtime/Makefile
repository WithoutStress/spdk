#  SPDX-License-Identifier: BSD-3-Clause

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = fsssd_runtime

C_SRCS := fsssd_runtime.c
CFLAGS += -I$(SPDK_ROOT_DIR)/module/fsdev/fsssd

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_fsdev

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
