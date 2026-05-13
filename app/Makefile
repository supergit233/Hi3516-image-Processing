
include ../Makefile.param
.DEFAULT_GOAL := all

SMP_SRCS := sample_venc.c sample_mp4_muxer.c
TARGET := sample_venc

TARGET_PATH := $(PWD)
RTSP_LIB_DIR := ../rtsp/rtsp_lib

CFLAGS += -I$(RTSP_LIB_DIR)
MPI_LIBS += $(RTSP_LIB_DIR)/librtsp.a

.PHONY: rtsp_lib
rtsp_lib:
	$(MAKE) -C $(RTSP_LIB_DIR) CC="$(CC)" AR="$(AR)"

$(TARGET): |rtsp_lib

# compile linux or LITEOS
include $(PWD)/../$(ARM_ARCH)_$(OSTYPE).mak
