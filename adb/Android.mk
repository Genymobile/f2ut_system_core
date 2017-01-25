# Copyright 2005 The Android Open Source Project
#
# Android.mk for adb
#

LOCAL_PATH:= $(call my-dir)

# adbd device daemon
# =========================================================

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	adb.c \
	fdevent.c \
	transport.c \
	transport_local.c \
	transport_usb.c \
	adb_auth_client.c \
	sockets.c \
	services.c \
	file_sync_service.c \
	jdwp_service.c \
	framebuffer_service.c \
	remount_service.c \
	disable_verity_service.c \
	usb_linux_client.c

ifeq ($(call is-vendor-board-platform,QCOM),true)
LOCAL_C_INCLUDES += $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr/include
LOCAL_ADDITIONAL_DEPENDENCIES := $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr
endif

LOCAL_CFLAGS := -O2 -g -DADB_HOST=0 -Wall -Wno-unused-parameter -Werror
LOCAL_CFLAGS += -D_XOPEN_SOURCE -D_GNU_SOURCE

ifneq (,$(filter userdebug eng,$(TARGET_BUILD_VARIANT)))
LOCAL_CFLAGS += -DALLOW_ADBD_ROOT=1
endif

ifneq (,$(filter userdebug,$(TARGET_BUILD_VARIANT)))
LOCAL_CFLAGS += -DALLOW_ADBD_DISABLE_VERITY=1
endif

LOCAL_MODULE := adbd

LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_MODULE_PATH := $(TARGET_ROOT_OUT_SBIN)
LOCAL_UNSTRIPPED_PATH := $(TARGET_ROOT_OUT_SBIN_UNSTRIPPED)
LOCAL_C_INCLUDES += system/extras/ext4_utils system/core/fs_mgr/include

LOCAL_STATIC_LIBRARIES := liblog \
	libfs_mgr \
	libcutils \
	libc \
	libmincrypt \
	libselinux \
	libext4_utils_static

include $(BUILD_EXECUTABLE)


# adb host tool for device-as-host
# =========================================================
ifneq ($(SDK_ONLY),true)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	adb.c \
	console.c \
	transport.c \
	transport_local.c \
	transport_usb.c \
	commandline.c \
	adb_client.c \
	adb_auth_host.c \
	sockets.c \
	services.c \
	file_sync_client.c \
	get_my_path_linux.c \
	usb_linux.c \
	fdevent.c

LOCAL_CFLAGS := \
	-O2 \
	-g \
	-DADB_HOST=1 \
	-DADB_HOST_ON_TARGET=1 \
	-Wall -Wno-unused-parameter -Werror \
	-D_XOPEN_SOURCE \
	-D_GNU_SOURCE

LOCAL_C_INCLUDES += external/openssl/include

LOCAL_MODULE := adb

LOCAL_STATIC_LIBRARIES := libzipfile libunz libcutils liblog

LOCAL_SHARED_LIBRARIES := libcrypto

include $(BUILD_EXECUTABLE)
endif
