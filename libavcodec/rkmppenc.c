/*
 * RockChip MPP Video Encoder
 * Copyright (c) 2018 hertz.wang@rock-chips.com
 * Copyright (c) 2023 jjm2473 at gmail.com
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
#include "libavutil/opt.h"

#include "codec_internal.h"
#include "hwconfig.h"

#define OFFSET(x) offsetof(RKMPPEncodeContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "profile",       "Set profile restrictions (h264_rkmpp)",    OFFSET(profile),       AV_OPT_TYPE_INT, { .i64=-1 }, 
            -1, FF_PROFILE_H264_HIGH, VE, "profile"},
        { "baseline",   NULL, 0, AV_OPT_TYPE_CONST, {.i64 = FF_PROFILE_H264_BASELINE},  INT_MIN, INT_MAX, VE, "profile" },
        { "main",       NULL, 0, AV_OPT_TYPE_CONST, {.i64 = FF_PROFILE_H264_MAIN},      INT_MIN, INT_MAX, VE, "profile" },
        { "high",       NULL, 0, AV_OPT_TYPE_CONST, {.i64 = FF_PROFILE_H264_HIGH},      INT_MIN, INT_MAX, VE, "profile" },
    { "8x8dct",        "High profile 8x8 transform (h264_rkmpp)", OFFSET(dct8x8), AV_OPT_TYPE_BOOL,   { .i64 = -1 }, -1, 1, VE},
    { NULL }
};

static const AVCodecHWConfigInternal *rkmpp_hw_configs[] = {
    HW_CONFIG_ENCODER_FRAMES(DRM_PRIME, DRM),
    NULL
};

#define RKMPP_ENC_CLASS(NAME) \
    static const AVClass rkmpp_##NAME##_enc_class = { \
        .class_name = "rkmpp_" #NAME "_enc", \
        .item_name  = av_default_item_name,\
        .option     = options, \
        .version    = LIBAVUTIL_VERSION_INT, \
    };

// TODO: .send_frame .receive_packet
#define RKMPP_ENC(NAME, ID, BSFS) \
    RKMPP_ENC_CLASS(NAME) \
    FFCodec ff_##NAME##_rkmpp_encoder = { \
        .p.name           = #NAME "_rkmpp", \
        .p.long_name      = NULL_IF_CONFIG_SMALL(#NAME " (rkmpp)"), \
        .p.type           = AVMEDIA_TYPE_VIDEO, \
        .p.id             = ID, \
        .init           = avrkmpp_init_encoder, \
        .close          = avrkmpp_close_encoder, \
        FF_CODEC_ENCODE_CB(avrkmpp_encode_frame), \
        .priv_data_size = sizeof(RKMPPEncodeContext), \
        .p.priv_class     = &rkmpp_##NAME##_enc_class, \
        .p.capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE, \
        .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_INIT_CLEANUP, \
        .p.pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_DRM_PRIME, \
                                                         AV_PIX_FMT_NONE }, \
        .hw_configs     = rkmpp_hw_configs, \
        .bsfs           = BSFS, \
        .p.wrapper_name   = "rkmpp", \
    };

RKMPP_ENC(h264, AV_CODEC_ID_H264, NULL)
