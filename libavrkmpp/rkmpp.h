
#ifndef AVRKMPP_RKMPP_H
#define AVRKMPP_RKMPP_H

#include "rkformat.h"
#include "libavcodec/codec_id.h"
#include <rockchip/rk_mpi.h>

static MppCodingType rkmpp_get_codingtype(enum AVCodecID codec)
{
    switch (codec) {
    case AV_CODEC_ID_H263:          return MPP_VIDEO_CodingH263;
    case AV_CODEC_ID_H264:          return MPP_VIDEO_CodingAVC;
    case AV_CODEC_ID_HEVC:          return MPP_VIDEO_CodingHEVC;
    case AV_CODEC_ID_AV1:           return MPP_VIDEO_CodingAV1;
    case AV_CODEC_ID_VP8:           return MPP_VIDEO_CodingVP8;
    case AV_CODEC_ID_VP9:           return MPP_VIDEO_CodingVP9;
    case AV_CODEC_ID_MPEG1VIDEO:    /* fallthrough */
    case AV_CODEC_ID_MPEG2VIDEO:    return MPP_VIDEO_CodingMPEG2;
    case AV_CODEC_ID_MPEG4:         return MPP_VIDEO_CodingMPEG4;
    default:                        return MPP_VIDEO_CodingUnused;
    }
}

#endif /* AVRKMPP_RKMPP_H */
