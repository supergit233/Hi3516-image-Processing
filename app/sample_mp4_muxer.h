/*
  Copyright (c), 2001-2024, Shenshu Tech. Co., Ltd.
 */

#ifndef SAMPLE_MP4_MUXER_H
#define SAMPLE_MP4_MUXER_H

#include "sample_comm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sample_mp4_muxer sample_mp4_muxer;

td_bool sample_mp4_muxer_is_supported(ot_payload_type payload);
td_s32 sample_mp4_muxer_open(sample_mp4_muxer **muxer, const td_char *file_name,
    ot_payload_type payload, td_u32 width, td_u32 height, td_u32 frame_rate);
td_s32 sample_mp4_muxer_write_stream(sample_mp4_muxer *muxer, const ot_venc_stream *stream);
td_bool sample_mp4_muxer_stream_is_key_frame(ot_payload_type payload, const ot_venc_stream *stream);
td_void sample_mp4_muxer_close(sample_mp4_muxer *muxer);

#ifdef __cplusplus
}
#endif

#endif
