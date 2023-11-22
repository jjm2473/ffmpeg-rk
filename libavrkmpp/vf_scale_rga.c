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

#include <string.h>
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/imgutils.h"
#include "libavutil/parseutils.h"

#include "formats.h"
#include "internal.h"
#include "scale_eval.h"
#include "video.h"

#include "avrkmpp.h"
#include "rkrga.h"

#include <rga/RgaApi.h>

typedef struct ScaleRGA {
    AVBufferRef *frame_group_ref;

    AVBufferRef *device_ref;
    AVBufferRef *hwframes_ref;
    const rkformat *in_fmt;
    const rkformat *out_fmt;
    rga_rect_t output;
    int color_space_mode;
    int passthrough;

    MppBufferGroup frame_group;

    AVFrame *sw_frame;
} ScaleRGA;

static void rga_release_buffer(void *opaque, uint8_t *data) {
    AVBufferRef *frame_group_ref = opaque;
    MppBuffer *bufferp = (MppBuffer*)data;
    MppBuffer buffer = *bufferp;
    mpp_buffer_put(buffer);
    av_free(bufferp);
    av_buffer_unref(&frame_group_ref);
}

static int ff_mpp_create_buffer(ScaleRGA *filter, int size, AVBufferRef **out) {
    int err;
    AVBufferRef *frame_group_ref;
    MppBuffer buffer = NULL;
    MppBuffer *bufferp = NULL;

    frame_group_ref = av_buffer_ref(filter->frame_group_ref);
    if (!frame_group_ref) {
        return AVERROR(ENOMEM);
    }
    bufferp = av_mallocz(sizeof(MppBuffer));
    if (!bufferp) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    err = mpp_buffer_get(filter->frame_group, &buffer, size);
    if (err) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    *bufferp = buffer;
    *out = av_buffer_create((uint8_t *)bufferp, sizeof(MppBuffer), rga_release_buffer,
                                       frame_group_ref, AV_BUFFER_FLAG_READONLY);
    if (!*out) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    return 0;

fail:
    if (bufferp)
        av_free(bufferp);
    if (buffer)
        mpp_buffer_put(buffer);
    av_buffer_unref(&frame_group_ref);
    return err;
}

static void rga_release_frame(void *opaque, uint8_t *data)
{
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)data;
    AVBufferRef *buffer_ref = (AVBufferRef *)opaque;
    av_free(desc);
    av_buffer_unref(&buffer_ref);
}

static int ff_rga_vpp_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    AVFilterLink *inlink   = avctx->inputs[0];
    ScaleRGAContext *ctx   = avctx->priv;
    ScaleRGA *filter = (ScaleRGA *)ctx->filter_ref->data;
    int linesizes[4];
    MppBuffer buffer;
    AVBufferRef *buffer_ref = NULL;
    AVHWFramesContext *output_frames;
    int err;

    if (!inlink->hw_frames_ctx) {
        err = av_image_fill_linesizes(linesizes, filter->in_fmt->av, FFALIGN(inlink->w, 2));
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "get linesize of %s failed %d\n", av_get_pix_fmt_name(filter->in_fmt->av), err);
            return err;
        }
        if (!(filter->sw_frame = av_frame_alloc())) {
            return AVERROR(ENOMEM);
        }
        filter->sw_frame->hw_frames_ctx = av_hwframe_ctx_alloc(avctx->hw_device_ctx);
        if (!filter->sw_frame->hw_frames_ctx) {
            av_log(ctx, AV_LOG_ERROR, "Failed to create HW frame context "
                "for upload.\n");
            err = AVERROR(ENOMEM);
            goto fail;
        }

        output_frames = (AVHWFramesContext*)filter->sw_frame->hw_frames_ctx->data;

        output_frames->format    = AV_PIX_FMT_DRM_PRIME;
        output_frames->sw_format = filter->in_fmt->av;
        output_frames->width     = FFALIGN(inlink->w, 2);
        output_frames->height    = FFALIGN(inlink->h, 2);

        err = av_hwframe_ctx_init(filter->sw_frame->hw_frames_ctx);
        if (err < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to initialise RGA frame "
                "context for upload: %d\n", err);
            goto fail;
        }

        err = ff_mpp_create_buffer(filter, 
            output_frames->width * output_frames->height * get_bpp_from_rga_format(filter->in_fmt->rga),
            &buffer_ref);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Failed to create mpp buffer for upload ret %d\n", err);
            goto fail;
        }
        buffer = *(MppBuffer*)buffer_ref->data;

        filter->sw_frame->width  = inlink->w >> 1 << 1;
        filter->sw_frame->height = inlink->h >> 1 << 1;
        err = rkmpp_map_frame(filter->sw_frame, filter->in_fmt, 
            mpp_buffer_get_fd(buffer), mpp_buffer_get_size(buffer),
            linesizes[0], output_frames->height,
            rga_release_frame, buffer_ref);
        if (err)
            goto fail;

    }

    return 0;

fail:
    av_buffer_unref(&buffer_ref);
    av_frame_free(&filter->sw_frame);
    return err;
}

int avrkmpp_scale_rga_config_input(AVFilterLink *inlink)
{
    int ret;
    AVFilterContext *avctx   = inlink->dst;
    ScaleRGAContext *ctx   = avctx->priv;
    ScaleRGA *filter = (ScaleRGA *)ctx->filter_ref->data;
    rga_rect_t *rect = &filter->output;
    AVHWFramesContext *output_frames = (AVHWFramesContext*)filter->hwframes_ref->data;;

    av_log(avctx, AV_LOG_DEBUG, "avrkmpp_scale_rga_config_input\n");

    if (inlink->hw_frames_ctx) {
        filter->in_fmt = rkmpp_get_av_format(((AVHWFramesContext*)inlink->hw_frames_ctx->data)->sw_format);
    } else {
        filter->in_fmt = rkmpp_get_av_format(inlink->format);
    }

    if (!filter->in_fmt) {
        av_log(ctx, AV_LOG_ERROR, "Unknown input pix format!\n");
        return AVERROR(EINVAL);
    }

    rect->width = ctx->width >> 1 << 1;
    rect->height = ctx->height >> 1 << 1;
    av_log(ctx, AV_LOG_DEBUG, "Final output video size w:%d h:%d\n", rect->width, rect->height);

    rect->wstride = FFALIGN(rect->width, 16);
    rect->hstride = rect->height;
    rect->xoffset = 0;
    rect->yoffset = 0;

    output_frames->width     = rect->width;
    output_frames->height    = rect->height;

    ret = av_hwframe_ctx_init(filter->hwframes_ref);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to initialise RGA frame "
               "context for output: %d\n", ret);
        return ret;
    }

    return 0;
}

int avrkmpp_scale_rga_config_output(AVFilterLink *outlink)
{
    AVFilterLink *inlink     = outlink->src->inputs[0];
    AVFilterContext *avctx   = outlink->src;
    ScaleRGAContext *ctx   = avctx->priv;
    ScaleRGA *filter = (ScaleRGA *)ctx->filter_ref->data;
    rga_rect_t *rect = &filter->output;
    int err;

    av_log(avctx, AV_LOG_DEBUG, "avrkmpp_scale_rga_config_output\n");

    outlink->w = rect->width;
    outlink->h = rect->height;
    outlink->format = AV_PIX_FMT_DRM_PRIME;

    av_log(ctx, AV_LOG_VERBOSE, "%s, %dx%d => %s, %dx%d\n",
        filter->in_fmt->av == AV_PIX_FMT_YUV420SPRK10 ? "yuv420sp10rk" : av_get_pix_fmt_name(filter->in_fmt->av),
        inlink->w, inlink->h,
        av_get_pix_fmt_name(filter->out_fmt->av), outlink->w, outlink->h);

    filter->color_space_mode = 0;
    if (ctx->hdr2sdr) {
        filter->color_space_mode = ff_rga_config_hdr2sdr(filter->in_fmt->rga, filter->out_fmt->rga);
        if (filter->color_space_mode) {
            av_log(ctx, AV_LOG_VERBOSE, "HDR to SDR mode %x\n", filter->color_space_mode);
        } else {
            av_log(ctx, AV_LOG_VERBOSE, "Unsupported or does not require HDR to SDR conversion\n");
        }
    }

    filter->passthrough = 0;
    if (inlink->hw_frames_ctx && outlink->w == inlink->w && outlink->h == inlink->h &&
            filter->in_fmt->rga == filter->out_fmt->rga && !filter->color_space_mode) {
        av_log(ctx, AV_LOG_VERBOSE, "Passthrough frames.\n");
        filter->passthrough = 1;
        av_buffer_unref(&outlink->hw_frames_ctx);
        outlink->hw_frames_ctx = av_buffer_ref(inlink->hw_frames_ctx);
        if (!outlink->hw_frames_ctx)
            return AVERROR(ENOMEM);
    } else if ((err = ff_rga_vpp_config_output(outlink)) < 0) {
        return err;
    }
    rect->size = rect->wstride * rect->hstride * get_bpp_from_rga_format(rect->format);

    return 0;
}

int avrkmpp_scale_rga_filter_frame(AVFilterLink *inlink, AVFrame *input_frame, AVFrame **output_frame0)
{

    AVFilterContext *avctx   = inlink->dst;
    AVFilterLink *outlink    = avctx->outputs[0];
    ScaleRGAContext *ctx   = avctx->priv;
    ScaleRGA *filter = (ScaleRGA *)ctx->filter_ref->data;
    rga_rect_t *rect = &filter->output;
    AVFrame *output_frame    = NULL;
    AVFrame *hw_frame = NULL;
    int err;
    MppBuffer buffer = NULL;
    int pitch0;
    AVBufferRef *buffer_ref = NULL;
    rga_info_t src_info = {0};
    rga_info_t dst_info = {0};

    if (filter->passthrough) {
        *output_frame0 = input_frame;
        return 0;
    }

    if (inlink->hw_frames_ctx) {
        hw_frame = input_frame;
    } else {
        const AVPixFmtDescriptor *pixdesc = av_pix_fmt_desc_get(input_frame->format);
        char *src_y = input_frame->data[0];
        char *src_u = input_frame->data[1];
        int y_pitch = input_frame->width;
        int src_height = input_frame->height;
        if (pixdesc->flags & AV_PIX_FMT_FLAG_PLANAR) {
            y_pitch = input_frame->linesize[0];
            src_height = (src_u - src_y) / y_pitch;
        }
        if (src_height < 0 || (src_height & 1) || (src_height>>1 > input_frame->height) || (y_pitch & 1)) {
            // RGA only supports continuous memory, and aligned to 2
            if ((err = av_hwframe_transfer_data(filter->sw_frame, input_frame, 0)) < 0)
                goto fail;

            if ((err = av_frame_copy_props(filter->sw_frame, input_frame)) < 0)
                goto fail;

            hw_frame = filter->sw_frame;
        } else {
            src_info.virAddr = src_y;
            rga_set_rect(&src_info.rect, 0, 0, input_frame->width >> 1 << 1, input_frame->height >> 1 << 1,
                y_pitch, src_height, filter->in_fmt->rga);
        }
    }
    if (hw_frame) {
        AVHWFramesContext *hwfctx = (AVHWFramesContext*)hw_frame->hw_frames_ctx->data;
        AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor*)hw_frame->data[0];
        rga_set_rect(&src_info.rect, 0, 0, hw_frame->width >> 1 << 1, hw_frame->height >> 1 << 1,
            hwfctx->width,
            hwfctx->height,
            filter->in_fmt->rga);
        src_info.fd = desc->objects[0].fd;
        src_info.virAddr = NULL;
    }
    src_info.mmuFlag = 1;

    err = ff_mpp_create_buffer(filter, rect->size, &buffer_ref);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create mpp buffer for output ret %d\n", err);
        goto fail;
    }
    buffer = *(MppBuffer*)buffer_ref->data;
    dst_info.fd = mpp_buffer_get_fd(buffer);
    dst_info.mmuFlag = 1;
    memcpy(&dst_info.rect, rect, sizeof(rga_rect_t));
    dst_info.color_space_mode = filter->color_space_mode;

    if ((err = c_RkRgaBlit(&src_info, &dst_info, NULL)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "RGA failed (code = %d)\n", err);
        err = AVERROR(EINVAL);
        goto fail;
    }

    pitch0 = rect->wstride;

    switch (filter->out_fmt->rga)
    {
    case RK_FORMAT_YCbCr_420_SP_10B:
    case RK_FORMAT_YCbCr_420_SP:
    case RK_FORMAT_YCbCr_422_SP:
    case RK_FORMAT_YCbCr_420_P:
    case RK_FORMAT_YCbCr_422_P:
        break;
    default:
        pitch0 = ceil(get_bpp_from_rga_format(filter->out_fmt->rga) * pitch0);
        break;
    }

    output_frame = av_frame_alloc();
    if (!output_frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = av_frame_copy_props(output_frame, input_frame);
    if (err < 0)
        goto fail;

    if (filter->color_space_mode) {
        output_frame->color_primaries = AVCOL_PRI_BT709;
        output_frame->color_trc = AVCOL_TRC_BT709;
        output_frame->colorspace = AVCOL_SPC_BT709;
        output_frame->color_range = AVCOL_RANGE_JPEG;
    }

    // setup general frame fields
    output_frame->width            = rect->width;
    output_frame->height           = rect->height;

    err = rkmpp_map_frame(output_frame, filter->out_fmt, dst_info.fd, rect->size,
        pitch0, rect->hstride,
        rga_release_frame, buffer_ref);

    if (err)
        goto fail;

    output_frame->hw_frames_ctx = av_buffer_ref(outlink->hw_frames_ctx);
    if (!output_frame->hw_frames_ctx) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    av_frame_free(&input_frame);

    *output_frame0 = output_frame;
    return 0;

fail:
    av_buffer_unref(&buffer_ref);
    av_frame_free(&output_frame);
    av_frame_free(&input_frame);
    return err;
}

static void rga_release_frame_group(void *opaque, uint8_t *data)
{
    MppBufferGroup fg = (MppBufferGroup)opaque;
    mpp_buffer_group_put(fg);
}

static av_cold void rkmpp_release_filter(void *opaque, uint8_t *data)
{
    ScaleRGA *filter = (ScaleRGA *)data;

    if (filter->sw_frame) {
        av_frame_free(&filter->sw_frame);
    }
    av_buffer_unref(&filter->frame_group_ref);
    av_buffer_unref(&filter->hwframes_ref);
    av_free(filter);
}

av_cold int avrkmpp_scale_rga_init(AVFilterContext *avctx)
{
    int ret;
    enum AVPixelFormat pix_fmt;
    AVHWFramesContext *output_frames;
    ScaleRGA *filter;
    ScaleRGAContext *ctx   = avctx->priv;
    ctx->filter_ref = NULL;
    av_log(avctx, AV_LOG_DEBUG, "avrkmpp_scale_rga_init\n");

    filter = av_mallocz(sizeof(ScaleRGA));
    if (!filter) {
        return AVERROR(ENOMEM);
    }

    ctx->filter_ref =
        av_buffer_create((uint8_t *)filter, sizeof(*filter),
                         rkmpp_release_filter, NULL, AV_BUFFER_FLAG_READONLY);
    if (!ctx->filter_ref) {
        av_free(filter);
        return AVERROR(ENOMEM);
    }

    if (ctx->pix_fmt) {
        pix_fmt = av_get_pix_fmt(ctx->pix_fmt);
        if (pix_fmt == AV_PIX_FMT_NONE) {
            av_log(ctx, AV_LOG_ERROR, "Unknown pix format %s!\n", ctx->pix_fmt);
            ret = AVERROR(EINVAL);
            goto fail;
        }
    } else {
        pix_fmt = AV_PIX_FMT_NV12;
    }
    filter->out_fmt = rkmpp_get_av_format(pix_fmt);
    if (!filter->out_fmt) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported pix format %s!\n", ctx->pix_fmt?ctx->pix_fmt:"");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    filter->output.format = filter->out_fmt->rga;

    if (ret = mpp_buffer_group_get_internal(&filter->frame_group, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_DMA32)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get buffer group (code = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    filter->frame_group_ref = av_buffer_create(NULL, 0, rga_release_frame_group,
                                               (void *)filter->frame_group, AV_BUFFER_FLAG_READONLY);
    if (!filter->frame_group_ref) {
        mpp_buffer_group_put(filter->frame_group);
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    avctx->hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
    if (!avctx->hw_device_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create HW device context "
               "for output.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = av_hwdevice_ctx_init(avctx->hw_device_ctx);
    if (ret < 0)
        goto fail;

    filter->hwframes_ref = av_hwframe_ctx_alloc(avctx->hw_device_ctx);
    if (!filter->hwframes_ref) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create HW frame context "
               "for output.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    output_frames = (AVHWFramesContext*)filter->hwframes_ref->data;

    output_frames->format    = AV_PIX_FMT_DRM_PRIME;
    output_frames->sw_format = filter->out_fmt->av;
    output_frames->width     = FFALIGN(ctx->width, 16);
    output_frames->height    = FFALIGN(ctx->height, 2);

    return 0;
fail:
    av_buffer_unref(&filter->hwframes_ref);
    av_buffer_unref(&ctx->filter_ref);
    return ret;
}

int avrkmpp_scale_rga_query_formats(AVFilterContext *avctx) {
    ScaleRGAContext *ctx   = avctx->priv;
    ScaleRGA *filter = (ScaleRGA *)ctx->filter_ref->data;
    av_log(avctx, AV_LOG_DEBUG, "avrkmpp_scale_rga_query_formats\n");
    avctx->outputs[0]->hw_frames_ctx = av_buffer_ref(filter->hwframes_ref);
    if (!avctx->outputs[0]->hw_frames_ctx) {
        return AVERROR(ENOMEM);
    }
    return 0;
}

void avrkmpp_scale_rga_uninit(AVFilterContext *avctx)
{
    ScaleRGAContext *ctx   = avctx->priv;
    av_log(avctx, AV_LOG_DEBUG, "avrkmpp_scale_rga_uninit\n");
    if (avctx->outputs && avctx->outputs[0]) {
        av_buffer_unref(&avctx->outputs[0]->hw_frames_ctx);
    }
    av_buffer_unref(&ctx->filter_ref);
}
