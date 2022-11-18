/*
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

/**
 * @file
 * Rockchip RGA based scale filter
 * @author: jjm2473 (jjm2473 at gmail.com)
 */

#include <drm_fourcc.h>

#include <string.h>
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/imgutils.h"
#include "libavutil/parseutils.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "scale_eval.h"
#include "video.h"

#include <rockchip/mpp_buffer.h>

#include <rga/rga.h>
#include <rga/RgaApi.h>

#ifndef DRM_FORMAT_NV12_10
#define DRM_FORMAT_NV12_10 fourcc_code('N', 'A', '1', '2')
#endif

#define RGA_SW_USE_IMAGE_ALLOC

typedef struct ScaleRGAContext {
    const AVClass *class;

    AVBufferRef *frame_group_ref;

    AVBufferRef *device_ref;
    AVBufferRef *hwframes_ref;
    rga_rect_t output;
    int passthrough;

    MppBufferGroup frame_group;

    AVFrame *sw_frame;

    char *w_expr;      // width expression string
    char *h_expr;      // height expression string
    char *size_str;    // WxH expression
    int force_original_aspect_ratio;
    int force_divisible_by;

    int down_scale_only;
} ScaleRGAContext;

typedef struct RGAFrameContext {
    AVBufferRef *frame_group_ref;
    MppBuffer buffer;
} RGAFrameContext;

static int ff_rga_query_formats(AVFilterContext *avctx)
{
    enum AVPixelFormat input_pix_fmts[] = {
        AV_PIX_FMT_DRM_PRIME,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NV12,
        // AV_PIX_FMT_P010,
        // AV_PIX_FMT_NV16,
        // AV_PIX_FMT_YUYV422,
        // AV_PIX_FMT_UYVY422,
        AV_PIX_FMT_NONE,
    };
    enum AVPixelFormat output_pix_fmts[] = {
        AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NONE,
    };
    int err;

    if ((err = ff_formats_ref(ff_make_format_list(input_pix_fmts),
                              &avctx->inputs[0]->outcfg.formats)) < 0)
        return err;
    if ((err = ff_formats_ref(ff_make_format_list(output_pix_fmts),
                              &avctx->outputs[0]->incfg.formats)) < 0)
        return err;

    return 0;
}

static int ff_rga_vpp_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    AVFilterLink *inlink   = avctx->inputs[0];
    ScaleRGAContext *ctx   = avctx->priv;
    rga_rect_t *rect = &ctx->output;
    AVHWFramesContext *output_frames;
    int err;

    ctx->hwframes_ref = av_hwframe_ctx_alloc(ctx->device_ref);
    if (!ctx->hwframes_ref) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create HW frame context "
               "for output.\n");
        return AVERROR(ENOMEM);
    }

    output_frames = (AVHWFramesContext*)ctx->hwframes_ref->data;

    output_frames->format    = AV_PIX_FMT_DRM_PRIME;
    output_frames->sw_format = AV_PIX_FMT_NV12;
    output_frames->width     = rect->width;
    output_frames->height    = rect->height;

    err = av_hwframe_ctx_init(ctx->hwframes_ref);
    if (err < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to initialise RGA frame "
               "context for output: %d\n", err);
        goto fail;
    }

    outlink->hw_frames_ctx = av_buffer_ref(ctx->hwframes_ref);
    if (!outlink->hw_frames_ctx) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    if (!inlink->hw_frames_ctx) {
        if (!(ctx->sw_frame = av_frame_alloc())) {
            err = AVERROR(ENOMEM);
            goto fail;
        }

        ctx->sw_frame->format = inlink->format;
        ctx->sw_frame->width  = FFALIGN(inlink->w, 2);
        ctx->sw_frame->height = FFALIGN(inlink->h, 2);
#ifdef RGA_SW_USE_IMAGE_ALLOC
        if ((err = av_image_alloc(ctx->sw_frame->data, ctx->sw_frame->linesize, 
            ctx->sw_frame->width, ctx->sw_frame->height, inlink->format, 32)) < 0)
            goto fail;
#else
        if ((err = av_frame_get_buffer(ctx->sw_frame, 0)) < 0)
            goto fail;

        /* avoid plane padding */
        if ((err = av_image_fill_pointers(ctx->sw_frame->data, ctx->sw_frame->format, 
                    FFALIGN(ctx->sw_frame->height, 32),
                    ctx->sw_frame->buf[0]->data, ctx->sw_frame->linesize)) < 0)
            goto fail;
#endif

    }

    return 0;

fail:
#ifdef RGA_SW_USE_IMAGE_ALLOC
    av_freep(&ctx->sw_frame->data[0]);
#endif
    av_frame_free(&ctx->sw_frame);
    av_buffer_unref(&ctx->hwframes_ref);
    return err;
}

static uint32_t ff_null_get_rgaformat(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_YUV420P:       return RK_FORMAT_YCbCr_420_P;
    case AV_PIX_FMT_NV12:          return RK_FORMAT_YCbCr_420_SP;
    case AV_PIX_FMT_P010:          return RK_FORMAT_YCbCr_420_SP_10B;
    case AV_PIX_FMT_NV16:          return RK_FORMAT_YCbCr_422_SP;
    case AV_PIX_FMT_YUYV422:       return RK_FORMAT_YUYV_422;
    case AV_PIX_FMT_UYVY422:       return RK_FORMAT_UYVY_422;
    default:                       return RK_FORMAT_UNKNOWN;
    }
}

static float get_bpp_from_rga_format(uint32_t rga_fmt) {
    switch (rga_fmt) {
    case RK_FORMAT_YCbCr_420_P:
    case RK_FORMAT_YCbCr_420_SP:
      return 1.5;
    case RK_FORMAT_YCbCr_420_SP_10B:
    case RK_FORMAT_YCbCr_422_SP:
    case RK_FORMAT_YUYV_422:
    case RK_FORMAT_UYVY_422:
      return 2.0;
    default:
      av_log(NULL, AV_LOG_WARNING, "unknown RGA format %d\n", rga_fmt);
      return 2.0;
    }
}

static int scale_rga_config_output(AVFilterLink *outlink)
{
    AVFilterLink *inlink     = outlink->src->inputs[0];
    AVFilterContext *avctx   = outlink->src;
    ScaleRGAContext *ctx   = avctx->priv;
    rga_rect_t *rect = &ctx->output;
    int err;

    if ((err = ff_scale_eval_dimensions(ctx,
                                        ctx->w_expr, ctx->h_expr,
                                        inlink, outlink,
                                        &rect->width, &rect->height)) < 0)
        return err;

    ff_scale_adjust_dimensions(inlink, &rect->width, &rect->height,
                               ctx->force_original_aspect_ratio, ctx->force_divisible_by);

    if ((ctx->down_scale_only == 1) && (rect->width > inlink->w || rect->height > inlink->h)) {
        rect->width = inlink->w;
        rect->height = inlink->h;
    }
    rect->width = rect->width >> 1 << 1;
    rect->height = rect->height >> 1 << 1;
    av_log(ctx, AV_LOG_DEBUG, "Final output video size w:%d h:%d\n", rect->width, rect->height);

    rect->wstride = rect->width;
    rect->hstride = rect->height;
    rect->xoffset = 0;
    rect->yoffset = 0;

    outlink->w = rect->width;
    outlink->h = rect->height;
    outlink->format = AV_PIX_FMT_DRM_PRIME;

    av_buffer_unref(&ctx->hwframes_ref);

    ctx->passthrough = 0;
    if (inlink->hw_frames_ctx && outlink->w == inlink->w && outlink->h == inlink->h &&
            ((AVHWFramesContext*)inlink->hw_frames_ctx->data)->sw_format == AV_PIX_FMT_NV12) {
        av_log(ctx, AV_LOG_VERBOSE, "Passthrough frames.\n");
        ctx->passthrough = 1;
        outlink->hw_frames_ctx = av_buffer_ref(inlink->hw_frames_ctx);
        if (!outlink->hw_frames_ctx)
            return AVERROR(ENOMEM);
    } else if ((err = ff_rga_vpp_config_output(outlink)) < 0) {
        return err;
    }
    rect->format = ff_null_get_rgaformat(AV_PIX_FMT_NV12);
    rect->size = rect->wstride * rect->hstride * get_bpp_from_rga_format(rect->format);

    if (inlink->sample_aspect_ratio.num)
        outlink->sample_aspect_ratio = av_mul_q((AVRational){outlink->h * inlink->w, outlink->w * inlink->h}, inlink->sample_aspect_ratio);
    else
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    return 0;
}

static void rga_release_frame(void *opaque, uint8_t *data)
{
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)data;
    RGAFrameContext *framecontext = (RGAFrameContext *)opaque;
    MppBuffer buffer = framecontext->buffer;
    av_free(desc);
    mpp_buffer_put(buffer);
    av_buffer_unref(&framecontext->frame_group_ref);
}

static uint32_t rga_get_drmformat(uint32_t rga_fmt) {
    switch (rga_fmt) {
    case RK_FORMAT_YCbCr_420_SP:        return DRM_FORMAT_NV12;
    case RK_FORMAT_YCbCr_420_SP_10B:    return DRM_FORMAT_NV12_10;
    case RK_FORMAT_YCbCr_422_SP:        return DRM_FORMAT_NV16;
    case RK_FORMAT_YUYV_422:            return DRM_FORMAT_YUYV;
    case RK_FORMAT_UYVY_422:            return DRM_FORMAT_UYVY;
    default:                            return 0;
    }
}

static int scale_rga_filter_frame(AVFilterLink *inlink, AVFrame *input_frame)
{

    AVFilterContext *avctx   = inlink->dst;
    AVFilterLink *outlink    = avctx->outputs[0];
    ScaleRGAContext *ctx   = avctx->priv;
    rga_rect_t *rect = &ctx->output;
    AVFrame *output_frame    = NULL;
    int err;
    MppBuffer buffer = NULL;
    AVDRMFrameDescriptor *desc;
    AVDRMLayerDescriptor *layer;
    AVBufferRef *frame_group_ref;
    RGAFrameContext *framecontext = NULL;
    rga_info_t src_info = {0};
    rga_info_t dst_info = {0};

    if (ctx->passthrough) {
        return ff_filter_frame(outlink, input_frame);
    }

    if (inlink->hw_frames_ctx) {
        desc = (AVDRMFrameDescriptor*)input_frame->data[0];
        layer = &desc->layers[0];
        rga_set_rect(&src_info.rect, 0, 0, input_frame->width >> 1 << 1, input_frame->height >> 1 << 1, 
            layer->planes[0].pitch, 
            layer->nb_planes > 1?(layer->planes[1].offset / layer->planes[0].pitch):input_frame->height,
            ff_null_get_rgaformat(((AVHWFramesContext*)input_frame->hw_frames_ctx->data)->sw_format));
        src_info.fd = desc->objects[0].fd;
    } else {
        char *src_y = input_frame->data[0];
        char *src_u = input_frame->data[1];
        int y_pitch = input_frame->linesize[0];
        int src_height = (src_u - src_y) / y_pitch;
        if (src_height < 0 || (src_height & 1) || (y_pitch & 1) || src_y + (src_height * y_pitch) != src_u) {
            src_y = ctx->sw_frame->data[0];
            src_u = ctx->sw_frame->data[1];
            y_pitch = ctx->sw_frame->linesize[0];
            src_height = (src_u - src_y) / y_pitch;

            if ((err = av_frame_copy(ctx->sw_frame, input_frame)) < 0)
                goto fail;

            if ((err = av_frame_copy_props(ctx->sw_frame, input_frame)) < 0)
                goto fail;
        }
        src_info.virAddr = src_y;
        rga_set_rect(&src_info.rect, 0, 0, input_frame->width >> 1 << 1, input_frame->height >> 1 << 1,
            y_pitch, src_height, ff_null_get_rgaformat(ctx->sw_frame->format));
    }
    src_info.mmuFlag = 1;

    frame_group_ref = av_buffer_ref(ctx->frame_group_ref);
    if (!frame_group_ref) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    err = mpp_buffer_get(ctx->frame_group, &buffer, rect->size);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get buffer for input frame ret %d\n", err);
        err = AVERROR(ENOMEM);
        goto fail;
    }
    dst_info.fd = mpp_buffer_get_fd(buffer);
    dst_info.mmuFlag = 1;
    memcpy(&dst_info.rect, rect, sizeof(rga_rect_t));

    if ((err = c_RkRgaBlit(&src_info, &dst_info, NULL)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "RGA failed (code = %d)\n", err);
        err = AVERROR(EINVAL);
        goto fail;
    }

    desc = av_mallocz(sizeof(AVDRMFrameDescriptor));
    if (!desc) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    desc->nb_objects = 1;
    desc->objects[0].fd = mpp_buffer_get_fd(buffer);
    desc->objects[0].size = mpp_buffer_get_size(buffer);

    desc->nb_layers = 1;
    layer = &desc->layers[0];
    layer->format = rga_get_drmformat(rect->format);
    layer->nb_planes = 2;

    layer->planes[0].object_index = 0;
    layer->planes[0].offset = 0;
    layer->planes[0].pitch = rect->wstride;

    layer->planes[1].object_index = 0;
    layer->planes[1].offset = layer->planes[0].pitch * rect->hstride;
    layer->planes[1].pitch = layer->planes[0].pitch;

    // frame group needs to be closed only when all frames have been released.
    framecontext = (RGAFrameContext *)av_mallocz(sizeof(RGAFrameContext));

    if (!framecontext) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    framecontext->frame_group_ref = frame_group_ref;
    framecontext->buffer = buffer;

    output_frame = av_frame_alloc();
    if (!output_frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = av_frame_copy_props(output_frame, input_frame);
    if (err < 0)
        goto fail;

    // setup general frame fields
    output_frame->format           = AV_PIX_FMT_DRM_PRIME;
    output_frame->width            = rect->width;
    output_frame->height           = rect->height;

    output_frame->data[0]  = (uint8_t *)desc;
    output_frame->buf[0]   = av_buffer_create((uint8_t *)desc, sizeof(*desc), rga_release_frame,
                                       framecontext, AV_BUFFER_FLAG_READONLY);

    if (!output_frame->buf[0]) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    output_frame->hw_frames_ctx = av_buffer_ref(outlink->hw_frames_ctx);
    if (!output_frame->hw_frames_ctx) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    av_frame_free(&input_frame);

    return ff_filter_frame(outlink, output_frame);

fail:
    av_frame_free(&output_frame);
    if (framecontext)
        av_free(framecontext);
    av_free(desc);
    mpp_buffer_put(buffer);
    if (frame_group_ref)
        av_buffer_unref(&frame_group_ref);
    av_frame_free(&input_frame);
    return err;
}

static av_cold int init_dict(AVFilterContext *ctx)
{
    ScaleRGAContext *scale = ctx->priv;
    rga_rect_t *rect = &scale->output;
    int ret;

    if (scale->size_str && (scale->w_expr || scale->h_expr)) {
        av_log(ctx, AV_LOG_ERROR,
               "Size and width/height expressions cannot be set at the same time.\n");
            return AVERROR(EINVAL);
    }

    if (scale->w_expr && !scale->h_expr)
        FFSWAP(char *, scale->w_expr, scale->size_str);

    if (scale->size_str) {
        char buf[32];
        if ((ret = av_parse_video_size(&rect->width, &rect->height, scale->size_str)) < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "Invalid size '%s'\n", scale->size_str);
            return ret;
        }
        snprintf(buf, sizeof(buf)-1, "%d", rect->width);
        av_opt_set(scale, "w", buf, 0);
        snprintf(buf, sizeof(buf)-1, "%d", rect->height);
        av_opt_set(scale, "h", buf, 0);
    }
    if (!scale->w_expr)
        av_opt_set(scale, "w", "iw", 0);
    if (!scale->h_expr)
        av_opt_set(scale, "h", "ih", 0);

    av_log(ctx, AV_LOG_VERBOSE, "Parsed expr w:%s h:%s\n",
           scale->w_expr, scale->h_expr);

    return 0;
}

static void rga_release_frame_group(void *opaque, uint8_t *data)
{
    MppBufferGroup fg = (MppBufferGroup)opaque;
    mpp_buffer_group_put(fg);
}

static av_cold int scale_rga_init(AVFilterContext *avctx)
{
    int ret;
    ScaleRGAContext *ctx   = avctx->priv;

    if (ret = init_dict(avctx))
        return ret;

    if (ret = mpp_buffer_group_get_internal(&ctx->frame_group, MPP_BUFFER_TYPE_DRM)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get buffer group (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    ctx->frame_group_ref = av_buffer_create(NULL, 0, rga_release_frame_group,
                                               (void *)ctx->frame_group, AV_BUFFER_FLAG_READONLY);
    if (!ctx->frame_group_ref) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ctx->output.format = RK_FORMAT_UNKNOWN;

    ctx->device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
    if (!ctx->device_ref) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = av_hwdevice_ctx_init(ctx->device_ref);
    if (ret < 0)
        goto fail;

    return 0;
fail:

    av_buffer_unref(&ctx->device_ref);
    mpp_buffer_group_put(ctx->frame_group);
    return ret;
}

static void rga_ctx_uninit(AVFilterContext *avctx)
{
    ScaleRGAContext *ctx   = avctx->priv;
    if (ctx->sw_frame) {
#ifdef RGA_SW_USE_IMAGE_ALLOC
        av_freep(&ctx->sw_frame->data[0]);
#endif
        av_frame_free(&ctx->sw_frame);
    }
    av_buffer_unref(&ctx->frame_group_ref);
    av_buffer_unref(&ctx->hwframes_ref);
    av_buffer_unref(&ctx->device_ref);
}

#define OFFSET(x) offsetof(ScaleRGAContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption scale_rga_options[] = {
    { "w", "output video width", OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = NULL}, .flags = FLAGS },
    { "h", "output video height", OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = NULL}, .flags = FLAGS },
    { "s", "output video size (WxH)", OFFSET(size_str), AV_OPT_TYPE_STRING, {.str = NULL}, .flags = FLAGS },
    { "force_original_aspect_ratio", "decrease or increase w/h if necessary to keep the original AR", 
            OFFSET(force_original_aspect_ratio), AV_OPT_TYPE_INT, { .i64 = 1}, 0, 2, FLAGS, "force_oar" },
        { "disable",  NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 0 }, 0, 0, FLAGS, "force_oar" },
        { "decrease", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 1 }, 0, 0, FLAGS, "force_oar" },
        { "increase", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 2 }, 0, 0, FLAGS, "force_oar" },
    { "force_divisible_by", "enforce that the output resolution is divisible by a defined integer when force_original_aspect_ratio is used", 
            OFFSET(force_divisible_by), AV_OPT_TYPE_INT, { .i64 = 1}, 1, 256, FLAGS },
    { "down_scale_only", "do not upscale", OFFSET(down_scale_only), AV_OPT_TYPE_BOOL, { .i64 = 1}, 0, 1, FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(scale_rga);

static const AVFilterPad scale_rga_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &scale_rga_filter_frame,
    },
};

static const AVFilterPad scale_rga_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = &scale_rga_config_output,
    },
};

const AVFilter ff_vf_scale_rga = {
    .name          = "scale_rga",
    .description   = NULL_IF_CONFIG_SMALL("Scale to/from RGA surfaces."),
    .priv_size     = sizeof(ScaleRGAContext),
    .priv_class    = &scale_rga_class,
    .init          = &scale_rga_init,
    .uninit        = &rga_ctx_uninit,
    FILTER_INPUTS(scale_rga_inputs),
    FILTER_OUTPUTS(scale_rga_outputs),
    FILTER_QUERY_FUNC(&ff_rga_query_formats),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
