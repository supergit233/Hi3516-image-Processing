/*
  Copyright (c), 2001-2024, Shenshu Tech. Co., Ltd.
 */

#include "sample_mp4_muxer.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_MP4_TIMESCALE 90000
#define SAMPLE_MP4_DEFAULT_FPS 30
#define SAMPLE_MP4_BOX_DEPTH_MAX 32
#define SAMPLE_MP4_SAMPLE_CAP_INIT 256

#define SAMPLE_MP4_H264_NALU_AUD 9
#define SAMPLE_MP4_H265_NALU_AUD 35
#define SAMPLE_MP4_H265_NALU_CRA 21

#define SAMPLE_MP4_CHECK_RETURN(expr) do { \
        if ((expr) != TD_SUCCESS) { \
            return TD_FAILURE; \
        } \
    } while (0)

typedef struct {
    td_u8 *data;
    td_u32 len;
} sample_mp4_nalu;

typedef struct {
    td_u64 offset;
    td_u32 size;
    td_u32 duration;
    td_bool key_frame;
} sample_mp4_sample;

typedef struct {
    td_u8 *data;
    td_u32 size;
    td_u32 capacity;
    td_u32 box_pos[SAMPLE_MP4_BOX_DEPTH_MAX];
    td_u32 depth;
} sample_mp4_buf;

struct sample_mp4_muxer {
    FILE *file;
    ot_payload_type payload;
    td_u32 width;
    td_u32 height;
    td_u32 frame_rate;
    td_u32 frame_delta;
    td_u64 file_pos;
    td_u64 mdat_start;
    td_u64 mdat_size_pos;
    sample_mp4_sample *samples;
    td_u32 sample_count;
    td_u32 sample_capacity;
    sample_mp4_nalu h264_sps;
    sample_mp4_nalu h264_pps;
    sample_mp4_nalu h265_vps;
    sample_mp4_nalu h265_sps;
    sample_mp4_nalu h265_pps;
};

static td_void sample_mp4_nalu_free(sample_mp4_nalu *nalu)
{
    if (nalu->data != TD_NULL) {
        free(nalu->data);
        nalu->data = TD_NULL;
    }
    nalu->len = 0;
}

static td_s32 sample_mp4_nalu_set(sample_mp4_nalu *nalu, const td_u8 *data, td_u32 len)
{
    td_u8 *tmp = TD_NULL;

    if (len == 0 || data == TD_NULL) {
        return TD_FAILURE;
    }
    if (nalu->data != TD_NULL && nalu->len == len && memcmp(nalu->data, data, len) == 0) {
        return TD_SUCCESS;
    }

    tmp = (td_u8 *)malloc(len);
    if (tmp == TD_NULL) {
        return TD_FAILURE;
    }
    (td_void)memcpy(tmp, data, len);

    sample_mp4_nalu_free(nalu);
    nalu->data = tmp;
    nalu->len = len;
    return TD_SUCCESS;
}

static td_s32 sample_mp4_file_write(sample_mp4_muxer *muxer, const td_void *data, td_u32 len)
{
    if (len == 0) {
        return TD_SUCCESS;
    }
    if (muxer == TD_NULL || muxer->file == TD_NULL || data == TD_NULL) {
        return TD_FAILURE;
    }
    if (fwrite(data, 1, len, muxer->file) != len) {
        return TD_FAILURE;
    }
    muxer->file_pos += len;
    return TD_SUCCESS;
}

static td_s32 sample_mp4_file_write_u32(sample_mp4_muxer *muxer, td_u32 value)
{
    td_u8 data[4]; /* 4 bytes */

    data[0] = (td_u8)((value >> 24) & 0xff); /* 24: bit offset */
    data[1] = (td_u8)((value >> 16) & 0xff); /* 16: bit offset */
    data[2] = (td_u8)((value >> 8) & 0xff);  /* 8: bit offset */
    data[3] = (td_u8)(value & 0xff);
    return sample_mp4_file_write(muxer, data, sizeof(data));
}

static td_s32 sample_mp4_file_write_u64(sample_mp4_muxer *muxer, td_u64 value)
{
    td_u8 data[8]; /* 8 bytes */
    td_u32 i;

    for (i = 0; i < sizeof(data); i++) {
        data[i] = (td_u8)((value >> ((7 - i) * 8)) & 0xff); /* 7/8: byte order */
    }
    return sample_mp4_file_write(muxer, data, sizeof(data));
}

static td_s32 sample_mp4_file_write_type(sample_mp4_muxer *muxer, const td_char type[4])
{
    return sample_mp4_file_write(muxer, type, 4); /* 4: box type len */
}

static td_s32 sample_mp4_patch_u64(sample_mp4_muxer *muxer, td_u64 offset, td_u64 value)
{
    td_u8 data[8]; /* 8 bytes */
    td_u32 i;
    long cur;

    if (muxer == TD_NULL || muxer->file == TD_NULL || offset > LONG_MAX) {
        return TD_FAILURE;
    }

    cur = (long)muxer->file_pos;
    for (i = 0; i < sizeof(data); i++) {
        data[i] = (td_u8)((value >> ((7 - i) * 8)) & 0xff); /* 7/8: byte order */
    }

    if (fseek(muxer->file, (long)offset, SEEK_SET) != 0) {
        return TD_FAILURE;
    }
    if (fwrite(data, 1, sizeof(data), muxer->file) != sizeof(data)) {
        return TD_FAILURE;
    }
    if (fseek(muxer->file, cur, SEEK_SET) != 0) {
        return TD_FAILURE;
    }
    return TD_SUCCESS;
}

static td_s32 sample_mp4_write_ftyp(sample_mp4_muxer *muxer)
{
    const td_char *codec_brand = (muxer->payload == OT_PT_H265) ? "hvc1" : "avc1";

    SAMPLE_MP4_CHECK_RETURN(sample_mp4_file_write_u32(muxer, 32)); /* 32: ftyp size */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_file_write_type(muxer, "ftyp"));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_file_write_type(muxer, "isom"));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_file_write_u32(muxer, 0x200)); /* minor version */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_file_write_type(muxer, "isom"));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_file_write_type(muxer, "iso2"));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_file_write_type(muxer, codec_brand));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_file_write_type(muxer, "mp41"));
    return TD_SUCCESS;
}

static td_s32 sample_mp4_write_mdat_header(sample_mp4_muxer *muxer)
{
    muxer->mdat_start = muxer->file_pos;
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_file_write_u32(muxer, 1)); /* 1: use large size */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_file_write_type(muxer, "mdat"));
    muxer->mdat_size_pos = muxer->file_pos;
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_file_write_u64(muxer, 0));
    return TD_SUCCESS;
}

static td_s32 sample_mp4_buf_reserve(sample_mp4_buf *buf, td_u32 len)
{
    td_u32 new_capacity;
    td_u8 *new_data = TD_NULL;

    if (len > 0xffffffffU - buf->size) {
        return TD_FAILURE;
    }
    if (buf->size + len <= buf->capacity) {
        return TD_SUCCESS;
    }

    new_capacity = (buf->capacity == 0) ? 1024 : buf->capacity; /* 1024: init buf size */
    while (new_capacity < buf->size + len) {
        if (new_capacity > 0x80000000U) {
            return TD_FAILURE;
        }
        new_capacity *= 2; /* 2: grow ratio */
    }

    new_data = (td_u8 *)realloc(buf->data, new_capacity);
    if (new_data == TD_NULL) {
        return TD_FAILURE;
    }
    buf->data = new_data;
    buf->capacity = new_capacity;
    return TD_SUCCESS;
}

static td_s32 sample_mp4_buf_put(sample_mp4_buf *buf, const td_void *data, td_u32 len)
{
    if (len == 0) {
        return TD_SUCCESS;
    }
    if (data == TD_NULL || sample_mp4_buf_reserve(buf, len) != TD_SUCCESS) {
        return TD_FAILURE;
    }
    (td_void)memcpy(buf->data + buf->size, data, len);
    buf->size += len;
    return TD_SUCCESS;
}

static td_s32 sample_mp4_buf_put_u8(sample_mp4_buf *buf, td_u8 value)
{
    return sample_mp4_buf_put(buf, &value, 1);
}

static td_s32 sample_mp4_buf_put_u16(sample_mp4_buf *buf, td_u16 value)
{
    td_u8 data[2]; /* 2 bytes */

    data[0] = (td_u8)((value >> 8) & 0xff); /* 8: bit offset */
    data[1] = (td_u8)(value & 0xff);
    return sample_mp4_buf_put(buf, data, sizeof(data));
}

static td_s32 sample_mp4_buf_put_u24(sample_mp4_buf *buf, td_u32 value)
{
    td_u8 data[3]; /* 3 bytes */

    data[0] = (td_u8)((value >> 16) & 0xff); /* 16: bit offset */
    data[1] = (td_u8)((value >> 8) & 0xff);  /* 8: bit offset */
    data[2] = (td_u8)(value & 0xff);
    return sample_mp4_buf_put(buf, data, sizeof(data));
}

static td_s32 sample_mp4_buf_put_u32(sample_mp4_buf *buf, td_u32 value)
{
    td_u8 data[4]; /* 4 bytes */

    data[0] = (td_u8)((value >> 24) & 0xff); /* 24: bit offset */
    data[1] = (td_u8)((value >> 16) & 0xff); /* 16: bit offset */
    data[2] = (td_u8)((value >> 8) & 0xff);  /* 8: bit offset */
    data[3] = (td_u8)(value & 0xff);
    return sample_mp4_buf_put(buf, data, sizeof(data));
}

static td_s32 sample_mp4_buf_put_u64(sample_mp4_buf *buf, td_u64 value)
{
    td_u8 data[8]; /* 8 bytes */
    td_u32 i;

    for (i = 0; i < sizeof(data); i++) {
        data[i] = (td_u8)((value >> ((7 - i) * 8)) & 0xff); /* 7/8: byte order */
    }
    return sample_mp4_buf_put(buf, data, sizeof(data));
}

static td_s32 sample_mp4_buf_put_type(sample_mp4_buf *buf, const td_char type[4])
{
    return sample_mp4_buf_put(buf, type, 4); /* 4: box type len */
}

static td_void sample_mp4_buf_patch_u32(sample_mp4_buf *buf, td_u32 pos, td_u32 value)
{
    buf->data[pos] = (td_u8)((value >> 24) & 0xff);     /* 24: bit offset */
    buf->data[pos + 1] = (td_u8)((value >> 16) & 0xff); /* 16: bit offset */
    buf->data[pos + 2] = (td_u8)((value >> 8) & 0xff);  /* 8: bit offset */
    buf->data[pos + 3] = (td_u8)(value & 0xff);
}

static td_s32 sample_mp4_begin_box(sample_mp4_buf *buf, const td_char type[4])
{
    if (buf->depth >= SAMPLE_MP4_BOX_DEPTH_MAX) {
        return TD_FAILURE;
    }
    buf->box_pos[buf->depth++] = buf->size;
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_type(buf, type));
    return TD_SUCCESS;
}

static td_s32 sample_mp4_begin_full_box(sample_mp4_buf *buf, const td_char type[4], td_u8 version, td_u32 flags)
{
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_box(buf, type));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, version));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u24(buf, flags));
    return TD_SUCCESS;
}

static td_s32 sample_mp4_end_box(sample_mp4_buf *buf)
{
    td_u32 start;
    td_u32 size;

    if (buf->depth == 0) {
        return TD_FAILURE;
    }
    start = buf->box_pos[--buf->depth];
    size = buf->size - start;
    sample_mp4_buf_patch_u32(buf, start, size);
    return TD_SUCCESS;
}

static td_s32 sample_mp4_put_matrix(sample_mp4_buf *buf)
{
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0x00010000));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0x00010000));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0x40000000));
    return TD_SUCCESS;
}

static td_u32 sample_mp4_get_duration(const sample_mp4_muxer *muxer)
{
    td_u64 duration = (td_u64)muxer->sample_count * muxer->frame_delta;

    if (duration > 0xffffffffU) {
        return 0xffffffffU;
    }
    return (td_u32)duration;
}

static td_s32 sample_mp4_write_mvhd(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    td_u32 i;

    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_full_box(buf, "mvhd", 0, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, SAMPLE_MP4_TIMESCALE));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, sample_mp4_get_duration(muxer)));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0x00010000));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0x0100));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_put_matrix(buf));
    for (i = 0; i < 6; i++) { /* 6: predefined count */
        SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    }
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 2)); /* 2: next track id */
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_tkhd(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_full_box(buf, "tkhd", 0, 0x000007));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 1)); /* 1: track id */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, sample_mp4_get_duration(muxer)));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_put_matrix(buf));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, muxer->width << 16));  /* 16: fixed point */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, muxer->height << 16)); /* 16: fixed point */
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_mdhd(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_full_box(buf, "mdhd", 0, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, SAMPLE_MP4_TIMESCALE));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, sample_mp4_get_duration(muxer)));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0x55c4));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0));
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_hdlr(sample_mp4_buf *buf)
{
    const td_char name[] = "VideoHandler";

    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_full_box(buf, "hdlr", 0, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_type(buf, "vide"));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put(buf, name, sizeof(name)));
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_vmhd(sample_mp4_buf *buf)
{
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_full_box(buf, "vmhd", 0, 1));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0));
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_dinf(sample_mp4_buf *buf)
{
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_box(buf, "dinf"));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_full_box(buf, "dref", 0, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 1)); /* 1: entry count */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_full_box(buf, "url ", 0, 1));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_end_box(buf));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_end_box(buf));
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_avcc(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    if (muxer->h264_sps.len < 4 || muxer->h264_pps.len == 0) { /* 4: avc profile bytes */
        return TD_FAILURE;
    }

    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_box(buf, "avcC"));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, 1));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, muxer->h264_sps.data[1]));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, muxer->h264_sps.data[2])); /* 2: compatibility index */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, muxer->h264_sps.data[3])); /* 3: level index */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, 0xff));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, 0xe1));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, (td_u16)muxer->h264_sps.len));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put(buf, muxer->h264_sps.data, muxer->h264_sps.len));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, 1));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, (td_u16)muxer->h264_pps.len));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put(buf, muxer->h264_pps.data, muxer->h264_pps.len));
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_hvcc_array(sample_mp4_buf *buf, td_u8 nal_type, const sample_mp4_nalu *nalu)
{
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, (td_u8)(0x80 | (nal_type & 0x3f))));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 1)); /* 1: nalu count */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, (td_u16)nalu->len));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put(buf, nalu->data, nalu->len));
    return TD_SUCCESS;
}

static td_s32 sample_mp4_write_hvcc(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    td_u8 profile = 1;
    td_u8 level = 120; /* 120: level 4.0 */
    td_u8 compat[4] = {0}; /* 4 bytes */
    td_u8 constraint[6] = {0}; /* 6 bytes */
    td_u16 avg_frame_rate;

    if (muxer->h265_vps.len < 18 || muxer->h265_sps.len == 0 || muxer->h265_pps.len == 0) { /* 18: PTL offset */
        return TD_FAILURE;
    }

    profile = muxer->h265_vps.data[6]; /* 6: PTL profile byte offset in VPS */
    (td_void)memcpy(compat, muxer->h265_vps.data + 7, sizeof(compat));      /* 7: PTL compat offset */
    (td_void)memcpy(constraint, muxer->h265_vps.data + 11, sizeof(constraint)); /* 11: PTL constraint offset */
    level = muxer->h265_vps.data[17]; /* 17: PTL level offset */
    avg_frame_rate = (td_u16)(muxer->frame_rate * 256); /* 256: HEVC avgFrameRate unit */

    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_box(buf, "hvcC"));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, 1));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, profile));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put(buf, compat, sizeof(compat)));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put(buf, constraint, sizeof(constraint)));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, level));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0xf000));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, 0xfc));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, 0xfd));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, 0xf8));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, 0xf8));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, avg_frame_rate));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, 0x0f));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u8(buf, 3)); /* 3: VPS/SPS/PPS arrays */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_hvcc_array(buf, OT_VENC_H265_NALU_VPS, &muxer->h265_vps));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_hvcc_array(buf, OT_VENC_H265_NALU_SPS, &muxer->h265_sps));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_hvcc_array(buf, OT_VENC_H265_NALU_PPS, &muxer->h265_pps));
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_sample_entry(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    td_u8 compressor_name[32] = {0}; /* 32: visual sample entry name len */

    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_box(buf, (muxer->payload == OT_PT_H265) ? "hvc1" : "avc1"));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 1)); /* 1: data reference index */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, (td_u16)muxer->width));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, (td_u16)muxer->height));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0x00480000));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0x00480000));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 1)); /* 1: frame count */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put(buf, compressor_name, sizeof(compressor_name)));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0x0018));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u16(buf, 0xffff));

    if (muxer->payload == OT_PT_H265) {
        SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_hvcc(buf, muxer));
    } else {
        SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_avcc(buf, muxer));
    }
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_stsd(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_full_box(buf, "stsd", 0, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 1)); /* 1: entry count */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_sample_entry(buf, muxer));
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_stts(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_full_box(buf, "stts", 0, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 1)); /* 1: entry count */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, muxer->sample_count));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, muxer->frame_delta));
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_stsc(sample_mp4_buf *buf)
{
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_full_box(buf, "stsc", 0, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 1)); /* 1: entry count */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 1)); /* 1: first chunk */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 1)); /* 1: samples per chunk */
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 1)); /* 1: sample description index */
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_stsz(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    td_u32 i;

    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_full_box(buf, "stsz", 0, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, muxer->sample_count));
    for (i = 0; i < muxer->sample_count; i++) {
        SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, muxer->samples[i].size));
    }
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_co64(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    td_u32 i;

    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_full_box(buf, "co64", 0, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, muxer->sample_count));
    for (i = 0; i < muxer->sample_count; i++) {
        SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u64(buf, muxer->samples[i].offset));
    }
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_stss(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    td_u32 i;
    td_u32 key_count = 0;

    for (i = 0; i < muxer->sample_count; i++) {
        if (muxer->samples[i].key_frame == TD_TRUE) {
            key_count++;
        }
    }
    if (key_count == 0) {
        return TD_SUCCESS;
    }

    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_full_box(buf, "stss", 0, 0));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, key_count));
    for (i = 0; i < muxer->sample_count; i++) {
        if (muxer->samples[i].key_frame == TD_TRUE) {
            SAMPLE_MP4_CHECK_RETURN(sample_mp4_buf_put_u32(buf, i + 1)); /* sample number is 1-based */
        }
    }
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_stbl(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_box(buf, "stbl"));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_stsd(buf, muxer));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_stts(buf, muxer));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_stsc(buf));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_stsz(buf, muxer));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_co64(buf, muxer));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_stss(buf, muxer));
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_minf(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_box(buf, "minf"));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_vmhd(buf));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_dinf(buf));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_stbl(buf, muxer));
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_mdia(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_box(buf, "mdia"));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_mdhd(buf, muxer));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_hdlr(buf));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_minf(buf, muxer));
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_trak(sample_mp4_buf *buf, const sample_mp4_muxer *muxer)
{
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_box(buf, "trak"));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_tkhd(buf, muxer));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_mdia(buf, muxer));
    return sample_mp4_end_box(buf);
}

static td_s32 sample_mp4_write_moov(sample_mp4_muxer *muxer)
{
    sample_mp4_buf buf = {0};
    td_s32 ret;

    SAMPLE_MP4_CHECK_RETURN(sample_mp4_begin_box(&buf, "moov"));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_mvhd(&buf, muxer));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_trak(&buf, muxer));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_end_box(&buf));

    ret = sample_mp4_file_write(muxer, buf.data, buf.size);
    if (buf.data != TD_NULL) {
        free(buf.data);
    }
    return ret;
}

static td_s32 sample_mp4_add_sample(sample_mp4_muxer *muxer, td_u64 offset, td_u32 size, td_bool key_frame)
{
    sample_mp4_sample *new_samples = TD_NULL;
    td_u32 new_capacity;

    if (muxer->sample_count >= muxer->sample_capacity) {
        new_capacity = (muxer->sample_capacity == 0) ? SAMPLE_MP4_SAMPLE_CAP_INIT : muxer->sample_capacity * 2;
        new_samples = (sample_mp4_sample *)realloc(muxer->samples, sizeof(sample_mp4_sample) * new_capacity);
        if (new_samples == TD_NULL) {
            return TD_FAILURE;
        }
        muxer->samples = new_samples;
        muxer->sample_capacity = new_capacity;
    }

    muxer->samples[muxer->sample_count].offset = offset;
    muxer->samples[muxer->sample_count].size = size;
    muxer->samples[muxer->sample_count].duration = muxer->frame_delta;
    muxer->samples[muxer->sample_count].key_frame = key_frame;
    muxer->sample_count++;
    return TD_SUCCESS;
}

static td_bool sample_mp4_find_start_code(const td_u8 *data, td_u32 len, td_u32 from,
    td_u32 *start, td_u32 *start_code_len)
{
    td_u32 i;

    if (data == TD_NULL || len < 3) { /* 3: min start code len */
        return TD_FALSE;
    }

    for (i = from; i + 3 <= len; i++) { /* 3: short start code len */
        if (data[i] != 0 || data[i + 1] != 0) {
            continue;
        }
        if (data[i + 2] == 1) { /* 2: third byte */
            *start = i;
            *start_code_len = 3; /* 3: short start code len */
            return TD_TRUE;
        }
        if (i + 4 <= len && data[i + 2] == 0 && data[i + 3] == 1) { /* 4: long start code len */
            *start = i;
            *start_code_len = 4; /* 4: long start code len */
            return TD_TRUE;
        }
    }
    return TD_FALSE;
}

static td_s32 sample_mp4_write_len_prefix(sample_mp4_muxer *muxer, td_u32 len)
{
    return sample_mp4_file_write_u32(muxer, len);
}

static td_s32 sample_mp4_write_h264_nalu(sample_mp4_muxer *muxer, const td_u8 *nalu, td_u32 len,
    td_u32 *sample_size, td_bool *key_frame)
{
    td_u8 nalu_type;

    if (len == 0) {
        return TD_SUCCESS;
    }
    nalu_type = nalu[0] & 0x1f;
    if (nalu_type == OT_VENC_H264_NALU_SPS) {
        return sample_mp4_nalu_set(&muxer->h264_sps, nalu, len);
    }
    if (nalu_type == OT_VENC_H264_NALU_PPS) {
        return sample_mp4_nalu_set(&muxer->h264_pps, nalu, len);
    }
    if (nalu_type == SAMPLE_MP4_H264_NALU_AUD) {
        return TD_SUCCESS;
    }
    if (nalu_type == OT_VENC_H264_NALU_IDR_SLICE) {
        *key_frame = TD_TRUE;
    }

    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_len_prefix(muxer, len));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_file_write(muxer, nalu, len));
    *sample_size += len + 4; /* 4: length prefix */
    return TD_SUCCESS;
}

static td_s32 sample_mp4_write_h265_nalu(sample_mp4_muxer *muxer, const td_u8 *nalu, td_u32 len,
    td_u32 *sample_size, td_bool *key_frame)
{
    td_u8 nalu_type;

    if (len < 2) { /* 2: hevc nalu header len */
        return TD_SUCCESS;
    }
    nalu_type = (nalu[0] >> 1) & 0x3f;
    if (nalu_type == OT_VENC_H265_NALU_VPS) {
        return sample_mp4_nalu_set(&muxer->h265_vps, nalu, len);
    }
    if (nalu_type == OT_VENC_H265_NALU_SPS) {
        return sample_mp4_nalu_set(&muxer->h265_sps, nalu, len);
    }
    if (nalu_type == OT_VENC_H265_NALU_PPS) {
        return sample_mp4_nalu_set(&muxer->h265_pps, nalu, len);
    }
    if (nalu_type == SAMPLE_MP4_H265_NALU_AUD) {
        return TD_SUCCESS;
    }
    if (nalu_type == OT_VENC_H265_NALU_IDR_SLICE || nalu_type == 20 || nalu_type == SAMPLE_MP4_H265_NALU_CRA) {
        *key_frame = TD_TRUE;
    }

    SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_len_prefix(muxer, len));
    SAMPLE_MP4_CHECK_RETURN(sample_mp4_file_write(muxer, nalu, len));
    *sample_size += len + 4; /* 4: length prefix */
    return TD_SUCCESS;
}

static td_s32 sample_mp4_write_one_nalu(sample_mp4_muxer *muxer, const td_u8 *nalu, td_u32 len,
    td_u32 *sample_size, td_bool *key_frame)
{
    if (muxer->payload == OT_PT_H265) {
        return sample_mp4_write_h265_nalu(muxer, nalu, len, sample_size, key_frame);
    }
    return sample_mp4_write_h264_nalu(muxer, nalu, len, sample_size, key_frame);
}

static td_s32 sample_mp4_write_annexb_sample(sample_mp4_muxer *muxer, const td_u8 *data, td_u32 len)
{
    td_u32 start = 0;
    td_u32 start_code_len = 0;
    td_u32 next_start = 0;
    td_u32 next_start_code_len = 0;
    td_u32 nalu_start;
    td_u32 nalu_end;
    td_u32 search_from;
    td_u32 sample_size = 0;
    td_u64 sample_offset = muxer->file_pos;
    td_bool key_frame = TD_FALSE;
    td_bool has_start_code;

    has_start_code = sample_mp4_find_start_code(data, len, 0, &start, &start_code_len);
    if (has_start_code != TD_TRUE) {
        SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_one_nalu(muxer, data, len, &sample_size, &key_frame));
        if (sample_size > 0) {
            return sample_mp4_add_sample(muxer, sample_offset, sample_size, key_frame);
        }
        return TD_SUCCESS;
    }

    while (TD_TRUE) {
        nalu_start = start + start_code_len;
        search_from = nalu_start;
        if (sample_mp4_find_start_code(data, len, search_from, &next_start, &next_start_code_len) == TD_TRUE) {
            nalu_end = next_start;
        } else {
            nalu_end = len;
        }

        while (nalu_end > nalu_start && data[nalu_end - 1] == 0) {
            nalu_end--;
        }
        if (nalu_end > nalu_start) {
            SAMPLE_MP4_CHECK_RETURN(sample_mp4_write_one_nalu(muxer, data + nalu_start, nalu_end - nalu_start,
                &sample_size, &key_frame));
        }

        if (next_start <= start || next_start >= len ||
            sample_mp4_find_start_code(data, len, search_from, &next_start, &next_start_code_len) != TD_TRUE) {
            break;
        }
        start = next_start;
        start_code_len = next_start_code_len;
    }

    if (sample_size > 0) {
        return sample_mp4_add_sample(muxer, sample_offset, sample_size, key_frame);
    }
    return TD_SUCCESS;
}

static td_bool sample_mp4_has_config(const sample_mp4_muxer *muxer)
{
    if (muxer->payload == OT_PT_H265) {
        return (muxer->h265_vps.len > 0 && muxer->h265_sps.len > 0 && muxer->h265_pps.len > 0) ?
            TD_TRUE : TD_FALSE;
    }
    return (muxer->h264_sps.len > 0 && muxer->h264_pps.len > 0) ? TD_TRUE : TD_FALSE;
}

td_bool sample_mp4_muxer_is_supported(ot_payload_type payload)
{
    return (payload == OT_PT_H264 || payload == OT_PT_H265) ? TD_TRUE : TD_FALSE;
}

static td_bool sample_mp4_nalu_is_key_frame(ot_payload_type payload, const td_u8 *nalu, td_u32 len)
{
    td_u8 nalu_type;

    if (payload == OT_PT_H264) {
        if (nalu == TD_NULL || len < 1) {
            return TD_FALSE;
        }
        nalu_type = nalu[0] & 0x1f;
        return (nalu_type == OT_VENC_H264_NALU_IDR_SLICE) ? TD_TRUE : TD_FALSE;
    }

    if (payload == OT_PT_H265) {
        if (nalu == TD_NULL || len < 2) { /* 2: h265 nalu header len */
            return TD_FALSE;
        }
        nalu_type = (nalu[0] >> 1) & 0x3f;
        if (nalu_type == OT_VENC_H265_NALU_IDR_SLICE || nalu_type == 20 || nalu_type == SAMPLE_MP4_H265_NALU_CRA) {
            return TD_TRUE;
        }
    }

    return TD_FALSE;
}

static td_bool sample_mp4_annexb_has_key_frame(ot_payload_type payload, const td_u8 *data, td_u32 len)
{
    td_u32 start = 0;
    td_u32 start_code_len = 0;
    td_u32 search_from;
    td_u32 next_start = 0;
    td_u32 next_start_code_len = 0;
    td_u32 nalu_start;
    td_u32 nalu_end;

    if (data == TD_NULL || len == 0) {
        return TD_FALSE;
    }

    if (sample_mp4_find_start_code(data, len, 0, &start, &start_code_len) != TD_TRUE) {
        return sample_mp4_nalu_is_key_frame(payload, data, len);
    }

    search_from = start + start_code_len;
    while (search_from < len) {
        nalu_start = search_from;
        if (sample_mp4_find_start_code(data, len, search_from, &next_start, &next_start_code_len) == TD_TRUE) {
            nalu_end = next_start;
        } else {
            nalu_end = len;
        }

        if (nalu_end > nalu_start && sample_mp4_nalu_is_key_frame(payload, data + nalu_start,
            nalu_end - nalu_start) == TD_TRUE) {
            return TD_TRUE;
        }

        if (nalu_end == len) {
            break;
        }
        search_from = next_start + next_start_code_len;
    }

    return TD_FALSE;
}

td_bool sample_mp4_muxer_stream_is_key_frame(ot_payload_type payload, const ot_venc_stream *stream)
{
    td_u32 i;
    td_u32 frame_len = 0;
    td_u32 copy_pos = 0;
    td_u32 pack_len;
    td_u8 *frame = TD_NULL;
    td_bool key_frame;

    if (sample_mp4_muxer_is_supported(payload) != TD_TRUE ||
        stream == TD_NULL || stream->pack == TD_NULL) {
        return TD_FALSE;
    }

    for (i = 0; i < stream->pack_cnt; i++) {
        if (stream->pack[i].len <= stream->pack[i].offset) {
            continue;
        }
        pack_len = stream->pack[i].len - stream->pack[i].offset;
        if (pack_len > 0xffffffffU - frame_len) {
            return TD_FALSE;
        }
        frame_len += pack_len;
    }

    if (frame_len == 0) {
        return TD_FALSE;
    }

    frame = (td_u8 *)malloc(frame_len);
    if (frame == TD_NULL) {
        return TD_FALSE;
    }

    for (i = 0; i < stream->pack_cnt; i++) {
        if (stream->pack[i].len <= stream->pack[i].offset) {
            continue;
        }
        pack_len = stream->pack[i].len - stream->pack[i].offset;
        (td_void)memcpy(frame + copy_pos, stream->pack[i].addr + stream->pack[i].offset, pack_len);
        copy_pos += pack_len;
    }

    key_frame = sample_mp4_annexb_has_key_frame(payload, frame, frame_len);
    free(frame);
    return key_frame;
}

td_s32 sample_mp4_muxer_open(sample_mp4_muxer **muxer, const td_char *file_name,
    ot_payload_type payload, td_u32 width, td_u32 height, td_u32 frame_rate)
{
    sample_mp4_muxer *new_muxer = TD_NULL;

    if (muxer == TD_NULL || file_name == TD_NULL || sample_mp4_muxer_is_supported(payload) != TD_TRUE) {
        return TD_FAILURE;
    }

    new_muxer = (sample_mp4_muxer *)calloc(1, sizeof(sample_mp4_muxer));
    if (new_muxer == TD_NULL) {
        return TD_FAILURE;
    }

    new_muxer->file = fopen(file_name, "wb+");
    if (new_muxer->file == TD_NULL) {
        free(new_muxer);
        return TD_FAILURE;
    }

    new_muxer->payload = payload;
    new_muxer->width = width;
    new_muxer->height = height;
    new_muxer->frame_rate = (frame_rate == 0) ? SAMPLE_MP4_DEFAULT_FPS : frame_rate;
    new_muxer->frame_delta = SAMPLE_MP4_TIMESCALE / new_muxer->frame_rate;

    if (sample_mp4_write_ftyp(new_muxer) != TD_SUCCESS ||
        sample_mp4_write_mdat_header(new_muxer) != TD_SUCCESS) {
        sample_mp4_muxer_close(new_muxer);
        return TD_FAILURE;
    }

    *muxer = new_muxer;
    return TD_SUCCESS;
}

td_s32 sample_mp4_muxer_write_stream(sample_mp4_muxer *muxer, const ot_venc_stream *stream)
{
    td_u32 i;
    td_u32 frame_len = 0;
    td_u32 copy_pos = 0;
    td_u32 pack_len;
    td_u8 *frame = TD_NULL;
    td_s32 ret;

    if (muxer == TD_NULL || stream == TD_NULL || stream->pack == TD_NULL) {
        return TD_FAILURE;
    }

    for (i = 0; i < stream->pack_cnt; i++) {
        if (stream->pack[i].len <= stream->pack[i].offset) {
            continue;
        }
        pack_len = stream->pack[i].len - stream->pack[i].offset;
        if (pack_len > 0xffffffffU - frame_len) {
            return TD_FAILURE;
        }
        frame_len += pack_len;
    }
    if (frame_len == 0) {
        return TD_SUCCESS;
    }

    frame = (td_u8 *)malloc(frame_len);
    if (frame == TD_NULL) {
        return TD_FAILURE;
    }
    for (i = 0; i < stream->pack_cnt; i++) {
        if (stream->pack[i].len <= stream->pack[i].offset) {
            continue;
        }
        pack_len = stream->pack[i].len - stream->pack[i].offset;
        (td_void)memcpy(frame + copy_pos, stream->pack[i].addr + stream->pack[i].offset, pack_len);
        copy_pos += pack_len;
    }

    ret = sample_mp4_write_annexb_sample(muxer, frame, frame_len);
    free(frame);
    return ret;
}

td_void sample_mp4_muxer_close(sample_mp4_muxer *muxer)
{
    td_u64 mdat_size;

    if (muxer == TD_NULL) {
        return;
    }

    if (muxer->file != TD_NULL) {
        mdat_size = muxer->file_pos - muxer->mdat_start;
        if (sample_mp4_patch_u64(muxer, muxer->mdat_size_pos, mdat_size) != TD_SUCCESS) {
            printf("mp4 patch mdat size failed\n");
        } else if (muxer->sample_count > 0 && sample_mp4_has_config(muxer) == TD_TRUE) {
            if (sample_mp4_write_moov(muxer) != TD_SUCCESS) {
                printf("mp4 write moov failed\n");
            }
        } else if (muxer->sample_count > 0) {
            printf("mp4 missing codec config, file may not be playable\n");
        }
        (td_void)fflush(muxer->file);
        (td_void)fclose(muxer->file);
        muxer->file = TD_NULL;
    }

    sample_mp4_nalu_free(&muxer->h264_sps);
    sample_mp4_nalu_free(&muxer->h264_pps);
    sample_mp4_nalu_free(&muxer->h265_vps);
    sample_mp4_nalu_free(&muxer->h265_sps);
    sample_mp4_nalu_free(&muxer->h265_pps);
    if (muxer->samples != TD_NULL) {
        free(muxer->samples);
    }
    free(muxer);
}
