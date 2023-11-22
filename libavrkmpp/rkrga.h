
#ifndef AVRKMPP_RKRGA_H
#define AVRKMPP_RKRGA_H

#include "rkformat.h"

static int ff_rga_config_hdr2sdr(RgaSURF_FORMAT in, RgaSURF_FORMAT out) {
    switch(out) {
        case RK_FORMAT_YCbCr_420_SP:
        case RK_FORMAT_YCbCr_420_P:
        case RK_FORMAT_YCrCb_420_P:
        case RK_FORMAT_YCrCb_420_SP:
        case RK_FORMAT_YCbCr_422_SP:
        case RK_FORMAT_YCbCr_422_P:
        case RK_FORMAT_YCrCb_422_SP:
        case RK_FORMAT_YCrCb_422_P:
        case RK_FORMAT_YCbCr_400:
        case RK_FORMAT_YVYU_422:
        case RK_FORMAT_VYUY_422:
        case RK_FORMAT_YUYV_422:
        case RK_FORMAT_UYVY_422:
        case RK_FORMAT_YVYU_420:
        case RK_FORMAT_VYUY_420:
        case RK_FORMAT_YUYV_420:
        case RK_FORMAT_UYVY_420:
            break;
        default:
            return 0;
    }
    switch(in) {
        case RK_FORMAT_RGB_565:
        case RK_FORMAT_RGBA_5551:
        case RK_FORMAT_RGBA_4444:
        case RK_FORMAT_BGR_565:
        case RK_FORMAT_BGRA_5551:
        case RK_FORMAT_BGRA_4444:
        case RK_FORMAT_ARGB_5551:
        case RK_FORMAT_ARGB_4444:
        case RK_FORMAT_ABGR_5551:
        case RK_FORMAT_ABGR_4444:
        case RK_FORMAT_BGR_888:
        case RK_FORMAT_RGB_888:
        case RK_FORMAT_RGBA_8888:
        case RK_FORMAT_RGBX_8888:
        case RK_FORMAT_BGRA_8888:
        case RK_FORMAT_BGRX_8888:
        case RK_FORMAT_ARGB_8888:
        case RK_FORMAT_XRGB_8888:
        case RK_FORMAT_ABGR_8888:
        case RK_FORMAT_XBGR_8888:
            return rgb2yuv_709_full;
            break;
        case RK_FORMAT_YCbCr_420_SP_10B:
        case RK_FORMAT_YCrCb_420_SP_10B:
        case RK_FORMAT_YCbCr_422_10b_SP:
        case RK_FORMAT_YCrCb_422_10b_SP:
            return yuv2yuv_709_full_2_601_full;
            break;
        default:
            return 0;
    }
    return 0;
}

static float get_bpp_from_rga_format(RgaSURF_FORMAT rga_fmt) {
    // copy from librga/core/RgaUtils.cpp get_bpp_from_format
    switch(rga_fmt) {
        case RK_FORMAT_YCbCr_400:
            return 1.0;
        case RK_FORMAT_YCbCr_420_SP:
        case RK_FORMAT_YCbCr_420_P:
        case RK_FORMAT_YCrCb_420_P:
        case RK_FORMAT_YCrCb_420_SP:
            return 1.5;
        case RK_FORMAT_RGB_565:
        case RK_FORMAT_RGBA_5551:
        case RK_FORMAT_RGBA_4444:
        case RK_FORMAT_BGR_565:
        case RK_FORMAT_BGRA_5551:
        case RK_FORMAT_BGRA_4444:
        case RK_FORMAT_ARGB_5551:
        case RK_FORMAT_ARGB_4444:
        case RK_FORMAT_ABGR_5551:
        case RK_FORMAT_ABGR_4444:
        case RK_FORMAT_YCbCr_422_SP:
        case RK_FORMAT_YCbCr_422_P:
        case RK_FORMAT_YCrCb_422_SP:
        case RK_FORMAT_YCrCb_422_P:
        /* yuyv */
        case RK_FORMAT_YVYU_422:
        case RK_FORMAT_VYUY_422:
        case RK_FORMAT_YUYV_422:
        case RK_FORMAT_UYVY_422:
        case RK_FORMAT_YVYU_420:
        case RK_FORMAT_VYUY_420:
        case RK_FORMAT_YUYV_420:
        case RK_FORMAT_UYVY_420:

        case RK_FORMAT_YCbCr_420_SP_10B:
        case RK_FORMAT_YCrCb_420_SP_10B:
            return 2.0;
        case RK_FORMAT_YCbCr_422_10b_SP:
        case RK_FORMAT_YCrCb_422_10b_SP:
            return 2.5;
        case RK_FORMAT_BGR_888:
        case RK_FORMAT_RGB_888:
            return 3.0;
        case RK_FORMAT_RGBA_8888:
        case RK_FORMAT_RGBX_8888:
        case RK_FORMAT_BGRA_8888:
        case RK_FORMAT_BGRX_8888:
        case RK_FORMAT_ARGB_8888:
        case RK_FORMAT_XRGB_8888:
        case RK_FORMAT_ABGR_8888:
        case RK_FORMAT_XBGR_8888:
            return 4.0;
        default:
            av_log(NULL, AV_LOG_WARNING, "unknown RGA format %d\n", rga_fmt);
            return 2.0;
    }
}

#if 0
static float get_ppb_plane0_from_rga_format(RgaSURF_FORMAT rga_fmt) {
    switch(rga_fmt) {
        case RK_FORMAT_YCbCr_400:
        case RK_FORMAT_YCbCr_420_SP:
        case RK_FORMAT_YCbCr_420_P:
        case RK_FORMAT_YCrCb_420_P:
        case RK_FORMAT_YCrCb_420_SP:
        case RK_FORMAT_YCbCr_422_SP:
        case RK_FORMAT_YCbCr_422_P:
        case RK_FORMAT_YCrCb_422_SP:
        case RK_FORMAT_YCrCb_422_P:
            return 1.0;
        case RK_FORMAT_YCbCr_420_SP_10B:
        case RK_FORMAT_YCrCb_420_SP_10B:
        case RK_FORMAT_YCbCr_422_10b_SP:
        case RK_FORMAT_YCrCb_422_10b_SP:
            // here should be 0.8, I don't known why 1.0 working for RGA
            return 1.0;
        case RK_FORMAT_RGB_565:
        case RK_FORMAT_RGBA_5551:
        case RK_FORMAT_RGBA_4444:
        case RK_FORMAT_BGR_565:
        case RK_FORMAT_BGRA_5551:
        case RK_FORMAT_BGRA_4444:
        case RK_FORMAT_ARGB_5551:
        case RK_FORMAT_ARGB_4444:
        case RK_FORMAT_ABGR_5551:
        case RK_FORMAT_ABGR_4444:

        /* yuyv */
        case RK_FORMAT_YVYU_422:
        case RK_FORMAT_VYUY_422:
        case RK_FORMAT_YUYV_422:
        case RK_FORMAT_UYVY_422:
        case RK_FORMAT_YVYU_420:
        case RK_FORMAT_VYUY_420:
        case RK_FORMAT_YUYV_420:
        case RK_FORMAT_UYVY_420:
            return 0.5;
        case RK_FORMAT_BGR_888:
        case RK_FORMAT_RGB_888:
            return 1.0 / 3.0;
        case RK_FORMAT_RGBA_8888:
        case RK_FORMAT_RGBX_8888:
        case RK_FORMAT_BGRA_8888:
        case RK_FORMAT_BGRX_8888:
        case RK_FORMAT_ARGB_8888:
        case RK_FORMAT_XRGB_8888:
        case RK_FORMAT_ABGR_8888:
        case RK_FORMAT_XBGR_8888:
            return 0.25;
        default:
            av_log(NULL, AV_LOG_WARNING, "unknown RGA format %d\n", rga_fmt);
            return 1.0;
    }
}
#endif

#endif /* AVRKMPP_RKRGA_H */
