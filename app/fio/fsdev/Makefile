#  SPDX-License-Identifier: BSD-3-Clause

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

FIO_PLUGIN := spdk_fsdev

C_SRCS = fio_plugin.c

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_fsdev

include $(SPDK_ROOT_DIR)/mk/spdk.fio.mk
