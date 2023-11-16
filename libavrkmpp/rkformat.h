#ifndef AVRKMPP_RKFORMAT_H
#define AVRKMPP_RKFORMAT_H

#include <rockchip/mpp_frame.h>
#include <rga/rga.h>
#include "libavutil/pixdesc.h"

typedef struct {
    enum AVPixelFormat av;
    MppFrameFormat mpp;
    uint32_t drm;
    RgaSURF_FORMAT rga;
} rkformat;

#define DEFINE_GETFORMAT(NAME, TYPE)\
const rkformat *rkmpp_get_##NAME##_format(TYPE informat);

DEFINE_GETFORMAT(drm, uint32_t)
DEFINE_GETFORMAT(mpp, MppFrameFormat)
DEFINE_GETFORMAT(rga, RgaSURF_FORMAT)
DEFINE_GETFORMAT(av, enum AVPixelFormat)

#undef DEFINE_GETFORMAT

#endif