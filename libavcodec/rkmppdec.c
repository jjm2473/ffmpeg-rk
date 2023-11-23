/*
 * RockChip MPP Video Decoder
 * Copyright (c) 2017 Lionel CHAZALLON
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavrkmpp/avrkmpp.h"

#include "codec_internal.h"
#include "decode.h"
#include "hwconfig.h"

static int rkmpp_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    return avrkmpp_receive_frame(avctx, frame, ff_decode_get_packet);
}

static const AVCodecHWConfigInternal *const rkmpp_hw_configs[] = {
    HW_CONFIG_INTERNAL(DRM_PRIME),
    NULL
};

#define RKMPP_DEC_CLASS(NAME) \
    static const AVClass rkmpp_##NAME##_dec_class = { \
        .class_name = "rkmpp_" #NAME "_dec", \
        .version    = LIBAVUTIL_VERSION_INT, \
    };

#define RKMPP_DEC(NAME, ID, BSFS) \
    RKMPP_DEC_CLASS(NAME) \
    const FFCodec ff_##NAME##_rkmpp_decoder = { \
        .p.name         = #NAME "_rkmpp", \
        .p.long_name    = NULL_IF_CONFIG_SMALL(#NAME " (rkmpp)"), \
        .p.type         = AVMEDIA_TYPE_VIDEO, \
        .p.id           = ID, \
        .priv_data_size = sizeof(RKMPPDecodeContext), \
        .init           = avrkmpp_init_decoder, \
        .close          = avrkmpp_close_decoder, \
        FF_CODEC_RECEIVE_FRAME_CB(rkmpp_receive_frame), \
        .flush          = avrkmpp_decoder_flush, \
        .p.priv_class   = &rkmpp_##NAME##_dec_class, \
        .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
        .caps_internal  = FF_CODEC_CAP_CONTIGUOUS_BUFFERS, \
        .p.pix_fmts     = (const enum AVPixelFormat[]) { AV_PIX_FMT_DRM_PRIME, \
                                                         AV_PIX_FMT_NONE}, \
        .hw_configs     = rkmpp_hw_configs, \
        .bsfs           = BSFS, \
        .p.wrapper_name = "rkmpp", \
        .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE, \
    };

RKMPP_DEC(h263,  AV_CODEC_ID_H263,          NULL)
RKMPP_DEC(h264,  AV_CODEC_ID_H264,          "h264_mp4toannexb")
RKMPP_DEC(hevc,  AV_CODEC_ID_HEVC,          "hevc_mp4toannexb")
RKMPP_DEC(av1,   AV_CODEC_ID_AV1,           NULL)
RKMPP_DEC(vp8,   AV_CODEC_ID_VP8,           NULL)
RKMPP_DEC(vp9,   AV_CODEC_ID_VP9,           NULL)
RKMPP_DEC(mpeg1, AV_CODEC_ID_MPEG1VIDEO,    NULL)
RKMPP_DEC(mpeg2, AV_CODEC_ID_MPEG2VIDEO,    NULL)
RKMPP_DEC(mpeg4, AV_CODEC_ID_MPEG4,         "mpeg4_unpack_bframes")
RKMPP_DEC(mjpeg, AV_CODEC_ID_MJPEG,         NULL)
