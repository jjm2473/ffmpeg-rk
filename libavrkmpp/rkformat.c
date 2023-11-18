#include <drm_fourcc.h>
#include "rkformat.h"

// HACK: Older BSP kernel use NA12 for NV15.
#ifndef DRM_FORMAT_NV15 // fourcc_code('N', 'V', '1', '5')
#define DRM_FORMAT_NV15 fourcc_code('N', 'A', '1', '2')
#endif

#define RK_FORMAT_NR 20

// librga/core/utils/drm_utils/src/drm_utils.cpp drm_fourcc_table
// rkmpp/mpp/vproc/rga/rga.cpp rga_fmt_map
static const rkformat rkformats[RK_FORMAT_NR+1] = {
        { .av = AV_PIX_FMT_NV12,    .mpp = MPP_FMT_YUV420SP,        .drm = DRM_FORMAT_NV12,     .rga = RK_FORMAT_YCbCr_420_SP},
        { .av = AV_PIX_FMT_YUV420SPRK10, .mpp = MPP_FMT_YUV420SP_10BIT, .drm = DRM_FORMAT_NV15, .rga = RK_FORMAT_YCbCr_420_SP_10B},
        { .av = AV_PIX_FMT_NV16,    .mpp = MPP_FMT_YUV422SP,        .drm = DRM_FORMAT_NV16,     .rga = RK_FORMAT_YCbCr_422_SP},
        { .av = AV_PIX_FMT_YUV420P, .mpp = MPP_FMT_YUV420P,         .drm = DRM_FORMAT_YUV420,   .rga = RK_FORMAT_YCbCr_420_P},
        { .av = AV_PIX_FMT_YUV422P, .mpp = MPP_FMT_YUV422P,         .drm = DRM_FORMAT_YUV422,   .rga = RK_FORMAT_YCbCr_422_P},
        { .av = AV_PIX_FMT_YUYV422, .mpp = MPP_FMT_YUV422_YUYV,     .drm = DRM_FORMAT_YUYV,     .rga = RK_FORMAT_YUYV_422},
        { .av = AV_PIX_FMT_UYVY422, .mpp = MPP_FMT_YUV422_UYVY,     .drm = DRM_FORMAT_UYVY,     .rga = RK_FORMAT_UYVY_422},
        { .av = AV_PIX_FMT_RGB565,  .mpp = MPP_FMT_BGR565,          .drm = DRM_FORMAT_RGB565,   .rga = RK_FORMAT_BGR_565},
        { .av = AV_PIX_FMT_BGR565,  .mpp = MPP_FMT_RGB565,          .drm = DRM_FORMAT_BGR565,   .rga = RK_FORMAT_RGB_565},
        { .av = AV_PIX_FMT_RGB24,   .mpp = MPP_FMT_BGR888,          .drm = DRM_FORMAT_RGB888,   .rga = RK_FORMAT_BGR_888},
        { .av = AV_PIX_FMT_BGR24,   .mpp = MPP_FMT_RGB888,          .drm = DRM_FORMAT_BGR888,   .rga = RK_FORMAT_RGB_888},
        { .av = AV_PIX_FMT_RGBA,    .mpp = MPP_FMT_ABGR8888,        .drm = DRM_FORMAT_RGBA8888, .rga = RK_FORMAT_ABGR_8888},
        { .av = AV_PIX_FMT_RGB0,    .mpp = MPP_FMT_ABGR8888,        .drm = DRM_FORMAT_RGBX8888, .rga = RK_FORMAT_XBGR_8888},
        { .av = AV_PIX_FMT_BGRA,    .mpp = MPP_FMT_ARGB8888,        .drm = DRM_FORMAT_BGRA8888, .rga = RK_FORMAT_ARGB_8888},
        { .av = AV_PIX_FMT_BGR0,    .mpp = MPP_FMT_ARGB8888,        .drm = DRM_FORMAT_BGRX8888, .rga = RK_FORMAT_XRGB_8888},
        { .av = AV_PIX_FMT_ARGB,    .mpp = MPP_FMT_BGRA8888,        .drm = DRM_FORMAT_ARGB8888, .rga = RK_FORMAT_BGRA_8888},
        { .av = AV_PIX_FMT_0RGB,    .mpp = MPP_FMT_BGRA8888,        .drm = DRM_FORMAT_XRGB8888, .rga = RK_FORMAT_BGRX_8888},
        { .av = AV_PIX_FMT_ABGR,    .mpp = MPP_FMT_RGBA8888,        .drm = DRM_FORMAT_ABGR8888, .rga = RK_FORMAT_RGBA_8888},
        { .av = AV_PIX_FMT_0BGR,    .mpp = MPP_FMT_RGBA8888,        .drm = DRM_FORMAT_XBGR8888, .rga = RK_FORMAT_RGBX_8888},
        { .av = AV_PIX_FMT_GRAY8,   .mpp = MPP_FMT_YUV400,          .drm = DRM_FORMAT_YUV420_8BIT, .rga = RK_FORMAT_YCbCr_400},
        { .av = AV_PIX_FMT_NONE,    .mpp = MPP_FMT_BUTT,            .drm = DRM_FORMAT_INVALID,  .rga = RK_FORMAT_UNKNOWN} // sentinel
};

#define GETFORMAT(NAME, TYPE)\
const rkformat *rkmpp_get_##NAME##_format(TYPE informat){ \
    for(int i=0; i < RK_FORMAT_NR; i++){ \
        if(rkformats[i].NAME == informat){ \
            return &rkformats[i];\
        }\
    }\
    return NULL;\
}

GETFORMAT(drm, uint32_t)
GETFORMAT(mpp, MppFrameFormat)
GETFORMAT(rga, RgaSURF_FORMAT)
GETFORMAT(av, enum AVPixelFormat)

#undef GETFORMAT
