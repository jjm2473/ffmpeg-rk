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

#include <drm_fourcc.h>
#include <pthread.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/rk_mpi.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#include "avcodec.h"
#include "decode.h"
#include "hwconfig.h"
#include "internal.h"
#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"

#if CONFIG_LIBRGA
#include <rga/rga.h>
#include <rga/RgaApi.h>
#endif

#ifndef DRM_FORMAT_NV12_10
#define DRM_FORMAT_NV12_10 fourcc_code('N', 'A', '1', '2')
#endif

#define FPS_UPDATE_INTERVAL     120

typedef struct {
    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup frame_group;

    int8_t eos;
    int8_t draining;

    AVPacket packet;
    AVBufferRef *frames_ref;
    AVBufferRef *device_ref;

    char print_fps;

    uint64_t last_fps_time;
    uint64_t frames;

    char sync;
} RKMPPDecoder;

typedef struct {
    AVClass *av_class;
    AVBufferRef *decoder_ref;
} RKMPPDecodeContext;

typedef struct {
    MppFrame frame;
    AVBufferRef *decoder_ref;
} RKMPPFrameContext;

static MppCodingType rkmpp_get_codingtype(AVCodecContext *avctx)
{
    switch (avctx->codec_id) {
    case AV_CODEC_ID_H263:          return MPP_VIDEO_CodingH263;
    case AV_CODEC_ID_H264:          return MPP_VIDEO_CodingAVC;
    case AV_CODEC_ID_HEVC:          return MPP_VIDEO_CodingHEVC;
    case AV_CODEC_ID_VP8:           return MPP_VIDEO_CodingVP8;
    case AV_CODEC_ID_VP9:           return MPP_VIDEO_CodingVP9;
    case AV_CODEC_ID_MPEG1VIDEO:    /* fallthrough */
    case AV_CODEC_ID_MPEG2VIDEO:    return MPP_VIDEO_CodingMPEG2;
    case AV_CODEC_ID_MPEG4:         return MPP_VIDEO_CodingMPEG4;
    default:                        return MPP_VIDEO_CodingUnused;
    }
}

static uint32_t rkmpp_get_frameformat(MppFrameFormat mppformat)
{
    switch (mppformat) {
    case MPP_FMT_YUV420SP:          return DRM_FORMAT_NV12;
    case MPP_FMT_YUV420SP_10BIT:    return DRM_FORMAT_NV12_10;
    case MPP_FMT_YUV422SP:          return DRM_FORMAT_NV16;
    default:                        return 0;
    }
}

static uint32_t rkmpp_get_avformat(MppFrameFormat mppformat)
{
    switch (mppformat) {
    case MPP_FMT_YUV420SP:          return AV_PIX_FMT_NV12;
    case MPP_FMT_YUV420SP_10BIT:    return AV_PIX_FMT_P010;
    case MPP_FMT_YUV422SP:          return AV_PIX_FMT_NV16;
    default:                        return 0;
    }
}

static uint32_t rkmpp_get_rgaformat(MppFrameFormat mppformat)
{
    switch (mppformat) {
    case MPP_FMT_YUV420SP:          return RK_FORMAT_YCbCr_420_SP;
    case MPP_FMT_YUV420SP_10BIT:    return RK_FORMAT_YCbCr_420_SP_10B;
    case MPP_FMT_YUV422SP:          return RK_FORMAT_YCbCr_422_SP;
    default:                        return RK_FORMAT_UNKNOWN;
    }
}

static int rkmpp_close_decoder(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;

    av_packet_unref(&decoder->packet);

    av_buffer_unref(&rk_context->decoder_ref);
    return 0;
}

static void rkmpp_release_decoder(void *opaque, uint8_t *data)
{
    RKMPPDecoder *decoder = (RKMPPDecoder *)data;

    if (decoder->mpi) {
        decoder->mpi->reset(decoder->ctx);
        mpp_destroy(decoder->ctx);
        decoder->ctx = NULL;
    }

    if (decoder->frame_group) {
        mpp_buffer_group_put(decoder->frame_group);
        decoder->frame_group = NULL;
    }

    av_buffer_unref(&decoder->frames_ref);
    av_buffer_unref(&decoder->device_ref);

    av_free(decoder);
}

static int rkmpp_prepare_decoder(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    MppPacket packet;
    int ret;

    // send extra data
    if (avctx->extradata_size) {
        ret = mpp_packet_init(&packet, avctx->extradata, avctx->extradata_size);
        if (ret < 0)
            return AVERROR_UNKNOWN;
        ret = decoder->mpi->decode_put_packet(decoder->ctx, packet);
        mpp_packet_deinit(&packet);
        if (ret < 0)
            return AVERROR_UNKNOWN;
    }

    if (getenv("FFMPEG_RKMPP_SYNC")) {
        // wait for decode result after feeding any packets
        decoder->sync = 1;
        ret = 1;
        decoder->mpi->control(decoder->ctx, MPP_DEC_SET_IMMEDIATE_OUT, &ret);
    }
    return 0;
}

static int rkmpp_init_decoder(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = NULL;
    MppCodingType codectype = MPP_VIDEO_CodingUnused;
    char *env;
    int ret;

    avctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;

    // create a decoder and a ref to it
    decoder = av_mallocz(sizeof(RKMPPDecoder));
    if (!decoder) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    env = getenv("FFMPEG_RKMPP_LOG_FPS");
    if (env != NULL)
        decoder->print_fps = !!atoi(env);

    rk_context->decoder_ref = av_buffer_create((uint8_t *)decoder, sizeof(*decoder), rkmpp_release_decoder,
                                               NULL, AV_BUFFER_FLAG_READONLY);
    if (!rk_context->decoder_ref) {
        av_free(decoder);
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    av_log(avctx, AV_LOG_DEBUG, "Initializing RKMPP decoder.\n");

    codectype = rkmpp_get_codingtype(avctx);
    if (codectype == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unknown codec type (%d).\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_check_support_format(MPP_CTX_DEC, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Codec type (%d) unsupported by MPP\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // Create the MPP context
    ret = mpp_create(&decoder->ctx, &decoder->mpi);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = 1;
    decoder->mpi->control(decoder->ctx, MPP_DEC_SET_PARSER_FAST_MODE, &ret);

    // initialize mpp
    ret = mpp_init(decoder->ctx, MPP_CTX_DEC, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_buffer_group_get_internal(&decoder->frame_group, MPP_BUFFER_TYPE_DRM);
    if (ret) {
       av_log(avctx, AV_LOG_ERROR, "Failed to get buffer group (code = %d)\n", ret);
       ret = AVERROR_UNKNOWN;
       goto fail;
    }

    ret = decoder->mpi->control(decoder->ctx, MPP_DEC_SET_EXT_BUF_GROUP, decoder->frame_group);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed to assign buffer group (code = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    decoder->mpi->control(decoder->ctx, MPP_DEC_SET_DISABLE_ERROR, NULL);

    ret = rkmpp_prepare_decoder(avctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to prepare decoder (code = %d)\n", ret);
        goto fail;
    }

    decoder->device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
    if (!decoder->device_ref) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ret = av_hwdevice_ctx_init(decoder->device_ref);
    if (ret < 0)
        goto fail;

    av_log(avctx, AV_LOG_DEBUG, "RKMPP decoder initialized successfully.\n");

    return 0;

fail:
    av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP decoder.\n");
    rkmpp_close_decoder(avctx);
    return ret;
}

static void rkmpp_release_frame(void *opaque, uint8_t *data)
{
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)data;
    AVBufferRef *framecontextref = (AVBufferRef *)opaque;
    RKMPPFrameContext *framecontext = (RKMPPFrameContext *)framecontextref->data;

    mpp_frame_deinit(&framecontext->frame);
    av_buffer_unref(&framecontext->decoder_ref);
    av_buffer_unref(&framecontextref);

    av_free(desc);
}

static int rkmpp_convert_frame(AVCodecContext *avctx, AVFrame *frame,
                               MppFrame mppframe, MppBuffer buffer)
{
    char *src = mpp_buffer_get_ptr(buffer);
    char *dst_y = frame->data[0];
    char *dst_u = frame->data[1];
    char *dst_v = frame->data[2];
    RgaSURF_FORMAT format = rkmpp_get_rgaformat(mpp_frame_get_fmt(mppframe));
    int width = mpp_frame_get_width(mppframe);
    int height = mpp_frame_get_height(mppframe);
    int hstride = mpp_frame_get_hor_stride(mppframe);
    int vstride = mpp_frame_get_ver_stride(mppframe);
    int y_pitch = frame->linesize[0];
    int u_pitch = frame->linesize[1];
    int v_pitch = frame->linesize[2];
    int i, j;

#if CONFIG_LIBRGA
    rga_info_t src_info = {0};
    rga_info_t dst_info = {0};
    int dst_height = (dst_u - dst_y) / y_pitch;

    static int rga_supported = 1;
    static int rga_inited = 0;

    if (!rga_supported)
        goto bail;

    if (!rga_inited) {
        if (c_RkRgaInit() < 0) {
            rga_supported = 0;
            av_log(avctx, AV_LOG_WARNING, "RGA not available\n");
            goto bail;
        }
        rga_inited = 1;
    }

    if (format == RK_FORMAT_UNKNOWN)
        goto bail;

    if (u_pitch != y_pitch / 2 || v_pitch != y_pitch / 2 ||
        dst_u != dst_y + y_pitch * dst_height ||
        dst_v != dst_u + u_pitch * dst_height / 2)
        goto bail;

    src_info.fd = mpp_buffer_get_fd(buffer);
    src_info.mmuFlag = 1;
    rga_set_rect(&src_info.rect, 0, 0, width, height, hstride, vstride,
                 format);

    dst_info.virAddr = dst_y;
    dst_info.mmuFlag = 1;
    rga_set_rect(&dst_info.rect, 0, 0, frame->width, frame->height,
                 y_pitch, dst_height, RK_FORMAT_YCbCr_420_P);

    if (c_RkRgaBlit(&src_info, &dst_info, NULL) < 0)
        goto bail;

    return 0;

bail:
#endif
    if (format != RK_FORMAT_YCbCr_420_SP) {
        av_log(avctx, AV_LOG_WARNING, "Unable to convert\n");
        return -1;
    }

    av_log(avctx, AV_LOG_WARNING, "Doing slow software conversion\n");

    for (i = 0; i < frame->height; i++)
        memcpy(dst_y + i * y_pitch, src + i * hstride, frame->width);

    src += hstride * vstride;

    for (i = 0; i < frame->height / 2; i++) {
        for (j = 0; j < frame->width; j++) {
            dst_u[j] = src[2 * j + 0];
            dst_v[j] = src[2 * j + 1];
        }
        dst_u += u_pitch;
        dst_v += v_pitch;
        src += hstride;
    }

    return 0;
}

static void rkmpp_update_fps(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    struct timeval tv;
    uint64_t curr_time;
    float fps;

    if (!decoder->print_fps)
        return;

    if (!decoder->last_fps_time) {
        gettimeofday(&tv, NULL);
        decoder->last_fps_time = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }

    if (++decoder->frames % FPS_UPDATE_INTERVAL)
        return;

    gettimeofday(&tv, NULL);
    curr_time = tv.tv_sec * 1000 + tv.tv_usec / 1000;

    fps = 1000.0f * FPS_UPDATE_INTERVAL / (curr_time - decoder->last_fps_time);
    decoder->last_fps_time = curr_time;

    av_log(avctx, AV_LOG_INFO,
           "[FFMPEG RKMPP] FPS: %6.1f || Frames: %" PRIu64 "\n",
           fps, decoder->frames);
}

static int rkmpp_get_frame(AVCodecContext *avctx, AVFrame *frame, int timeout)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    RKMPPFrameContext *framecontext = NULL;
    AVBufferRef *framecontextref = NULL;
    int ret;
    MppFrame mppframe = NULL;
    MppBuffer buffer = NULL;
    AVDRMFrameDescriptor *desc = NULL;
    AVDRMLayerDescriptor *layer = NULL;
    int mode;
    MppFrameFormat mppformat;
    uint32_t drmformat;

    // should not provide any frame after EOS
    if (decoder->eos)
        return AVERROR_EOF;

    decoder->mpi->control(decoder->ctx, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&timeout);

    ret = decoder->mpi->decode_get_frame(decoder->ctx, &mppframe);
    if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get frame (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    if (!mppframe) {
        av_log(avctx, AV_LOG_DEBUG, "Timeout getting decoded frame.\n");
        return AVERROR(EAGAIN);
    }

    if (mpp_frame_get_eos(mppframe)) {
        av_log(avctx, AV_LOG_DEBUG, "Received a EOS frame.\n");
        decoder->eos = 1;
        ret = AVERROR_EOF;
        goto fail;
    }

    if (mpp_frame_get_discard(mppframe)) {
        av_log(avctx, AV_LOG_DEBUG, "Received a discard frame.\n");
        ret = AVERROR(EAGAIN);
        goto fail;
    }

    if (mpp_frame_get_errinfo(mppframe)) {
        av_log(avctx, AV_LOG_ERROR, "Received a errinfo frame.\n");
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    if (mpp_frame_get_info_change(mppframe)) {
        AVHWFramesContext *hwframes;

        av_log(avctx, AV_LOG_INFO, "Decoder noticed an info change (%dx%d), stride(%dx%d), format=%d\n",
               (int)mpp_frame_get_width(mppframe), (int)mpp_frame_get_height(mppframe),
               (int)mpp_frame_get_hor_stride(mppframe), (int)mpp_frame_get_ver_stride(mppframe), 
               (int)mpp_frame_get_fmt(mppframe));

        avctx->width = mpp_frame_get_width(mppframe);
        avctx->height = mpp_frame_get_height(mppframe);

        // chromium would align planes' width and height to 32, adding this
        // hack to avoid breaking the plane buffers' contiguous.
        avctx->coded_width = FFALIGN(avctx->width, 64);
        avctx->coded_height = FFALIGN(avctx->height, 64);

        decoder->mpi->control(decoder->ctx, MPP_DEC_SET_FRAME_INFO, (MppParam) mppframe);
        decoder->mpi->control(decoder->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

        av_buffer_unref(&decoder->frames_ref);

        decoder->frames_ref = av_hwframe_ctx_alloc(decoder->device_ref);
        if (!decoder->frames_ref) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        av_log(avctx, AV_LOG_VERBOSE, "hw_frames_ctx->data=%p\n", decoder->frames_ref->data);

        mppformat = mpp_frame_get_fmt(mppframe);

        hwframes = (AVHWFramesContext*)decoder->frames_ref->data;
        hwframes->format    = AV_PIX_FMT_DRM_PRIME;
        hwframes->sw_format = rkmpp_get_avformat(mppformat);
        hwframes->width     = avctx->width;
        hwframes->height    = avctx->height;
        ret = av_hwframe_ctx_init(decoder->frames_ref);
        if (!ret)
            ret = AVERROR(EAGAIN);

        goto fail;
    }

    // here we should have a valid frame
    av_log(avctx, AV_LOG_DEBUG, "Received a frame.\n");

    // now setup the frame buffer info
    buffer = mpp_frame_get_buffer(mppframe);
    if (!buffer) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get the frame buffer, frame is dropped (code = %d)\n", ret);
        ret = AVERROR(EAGAIN);
        goto fail;
    }

    rkmpp_update_fps(avctx);

/*
    if (avctx->pix_fmt != AV_PIX_FMT_DRM_PRIME) {
        av_log(avctx, AV_LOG_DEBUG, "Not drm prime.\n");
        ret = ff_get_buffer(avctx, frame, 0);
        if (ret < 0)
            goto out;
    }
*/
    // setup general frame fields
    frame->format           = AV_PIX_FMT_DRM_PRIME;
    frame->width            = mpp_frame_get_width(mppframe);
    frame->height           = mpp_frame_get_height(mppframe);
    frame->pts              = mpp_frame_get_pts(mppframe);
#if FF_API_PKT_PTS
    FF_DISABLE_DEPRECATION_WARNINGS;
    frame->pkt_pts          = frame->pts;
    FF_ENABLE_DEPRECATION_WARNINGS;
#endif
    frame->reordered_opaque = frame->pts;
    frame->color_range      = mpp_frame_get_color_range(mppframe);
    frame->color_primaries  = mpp_frame_get_color_primaries(mppframe);
    frame->color_trc        = mpp_frame_get_color_trc(mppframe);
    frame->colorspace       = mpp_frame_get_colorspace(mppframe);

    mode = mpp_frame_get_mode(mppframe);
    frame->interlaced_frame = ((mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) == MPP_FRAME_FLAG_DEINTERLACED);
    frame->top_field_first  = ((mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) == MPP_FRAME_FLAG_TOP_FIRST);

/*
    if (avctx->pix_fmt != AV_PIX_FMT_DRM_PRIME) {
        ret = rkmpp_convert_frame(avctx, frame, mppframe, buffer);
        goto out;
    }
*/

    mppformat = mpp_frame_get_fmt(mppframe);
    drmformat = rkmpp_get_frameformat(mppformat);

    desc = av_mallocz(sizeof(AVDRMFrameDescriptor));
    if (!desc) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    desc->nb_objects = 1;
    desc->objects[0].fd = mpp_buffer_get_fd(buffer);
    desc->objects[0].size = mpp_buffer_get_size(buffer);

    desc->nb_layers = 1;
    layer = &desc->layers[0];
    layer->format = drmformat;
    layer->nb_planes = 2;

    layer->planes[0].object_index = 0;
    layer->planes[0].offset = 0;
    layer->planes[0].pitch = mpp_frame_get_hor_stride(mppframe);

    layer->planes[1].object_index = 0;
    layer->planes[1].offset = layer->planes[0].pitch * mpp_frame_get_ver_stride(mppframe);
    layer->planes[1].pitch = layer->planes[0].pitch;

    // we also allocate a struct in buf[0] that will allow to hold additionnal information
    // for releasing properly MPP frames and decoder
    framecontextref = av_buffer_allocz(sizeof(*framecontext));
    if (!framecontextref) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    // MPP decoder needs to be closed only when all frames have been released.
    framecontext = (RKMPPFrameContext *)framecontextref->data;
    framecontext->decoder_ref = av_buffer_ref(rk_context->decoder_ref);
    framecontext->frame = mppframe;

    frame->data[0]  = (uint8_t *)desc;
    frame->buf[0]   = av_buffer_create((uint8_t *)desc, sizeof(*desc), rkmpp_release_frame,
                                       framecontextref, AV_BUFFER_FLAG_READONLY);

    if (!frame->buf[0]) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    frame->hw_frames_ctx = av_buffer_ref(decoder->frames_ref);
    if (!frame->hw_frames_ctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    return 0;

out:
fail:
    if (mppframe)
        mpp_frame_deinit(&mppframe);

    if (framecontext)
        av_buffer_unref(&framecontext->decoder_ref);

    if (framecontextref)
        av_buffer_unref(&framecontextref);

    if (desc)
        av_free(desc);

    return ret;
}

static int rkmpp_send_packet(AVCodecContext *avctx, AVPacket *packet)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    MppPacket mpkt;
    int64_t pts = packet->pts;
    int ret;

    // avoid sending new data after EOS
    if (decoder->draining)
        return AVERROR_EOF;

    if (!pts || pts == AV_NOPTS_VALUE)
        pts = avctx->reordered_opaque;

    ret = mpp_packet_init(&mpkt, packet->data, packet->size);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init MPP packet (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    mpp_packet_set_pts(mpkt, pts);

    ret = decoder->mpi->decode_put_packet(decoder->ctx, mpkt);
    mpp_packet_deinit(&mpkt);

    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_DEBUG, "Buffer full\n");
        return AVERROR(EAGAIN);
    }

    av_log(avctx, AV_LOG_DEBUG, "Wrote %d bytes to decoder\n", packet->size);
    return 0;
}

static int rkmpp_send_eos(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    MppPacket mpkt;
    int ret;

    ret = mpp_packet_init(&mpkt, NULL, 0);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init EOS packet (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    mpp_packet_set_eos(mpkt);

    do {
        ret = decoder->mpi->decode_put_packet(decoder->ctx, mpkt);
    } while (ret != MPP_OK);
    mpp_packet_deinit(&mpkt);

    decoder->draining = 1;

    return 0;
}

static int rkmpp_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    AVPacket *packet = &decoder->packet;
    int ret;

    // no more frames after EOS
    if (decoder->eos)
        return AVERROR_EOF;

    // draining remain frames
    if (decoder->draining)
        return rkmpp_get_frame(avctx, frame, MPP_TIMEOUT_BLOCK);

    while (1) {
        if (!packet->size) {
            ret = ff_decode_get_packet(avctx, packet);
            if (ret == AVERROR_EOF) {
                av_log(avctx, AV_LOG_DEBUG, "End of stream.\n");
                // send EOS and start draining
                rkmpp_send_eos(avctx);
                return rkmpp_get_frame(avctx, frame, MPP_TIMEOUT_BLOCK);
            } else if (ret == AVERROR(EAGAIN)) {
                // not blocking so that we can feed new data ASAP
                return rkmpp_get_frame(avctx, frame, MPP_TIMEOUT_NON_BLOCK);
            } else if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to get packet (code = %d)\n", ret);
                return ret;
            }
        } else {
            // send pending data to decoder
            ret = rkmpp_send_packet(avctx, packet);
            if (ret == AVERROR(EAGAIN)) {
                // some streams might need more packets to start returning frames
                ret = rkmpp_get_frame(avctx, frame, 5);
                if (ret != AVERROR(EAGAIN))
                    return ret;
            } else if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to send data (code = %d)\n", ret);
                return ret;
            } else {
                av_packet_unref(packet);
                packet->size = 0;

                // blocked waiting for decode result
                if (decoder->sync)
                    return rkmpp_get_frame(avctx, frame, MPP_TIMEOUT_BLOCK);
            }
        }
    }
}

static void rkmpp_flush(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;

    av_log(avctx, AV_LOG_DEBUG, "Flush.\n");

    decoder->mpi->reset(decoder->ctx);

    rkmpp_prepare_decoder(avctx);

    decoder->eos = 0;
    decoder->draining = 0;
    decoder->last_fps_time = decoder->frames = 0;

    av_packet_unref(&decoder->packet);
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
    const AVCodec ff_##NAME##_rkmpp_decoder = { \
        .name           = #NAME "_rkmpp", \
        .long_name      = NULL_IF_CONFIG_SMALL(#NAME " (rkmpp)"), \
        .type           = AVMEDIA_TYPE_VIDEO, \
        .id             = ID, \
        .priv_data_size = sizeof(RKMPPDecodeContext), \
        .init           = rkmpp_init_decoder, \
        .close          = rkmpp_close_decoder, \
        .receive_frame  = rkmpp_receive_frame, \
        .flush          = rkmpp_flush, \
        .priv_class     = &rkmpp_##NAME##_dec_class, \
        .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
        .caps_internal  = FF_CODEC_CAP_CONTIGUOUS_BUFFERS, \
        .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_DRM_PRIME, \
                                                         AV_PIX_FMT_NONE}, \
        .hw_configs     = rkmpp_hw_configs, \
        .bsfs           = BSFS, \
        .wrapper_name   = "rkmpp", \
    };

RKMPP_DEC(h263,  AV_CODEC_ID_H263,          NULL)
RKMPP_DEC(h264,  AV_CODEC_ID_H264,          "h264_mp4toannexb")
RKMPP_DEC(hevc,  AV_CODEC_ID_HEVC,          "hevc_mp4toannexb")
RKMPP_DEC(vp8,   AV_CODEC_ID_VP8,           NULL)
RKMPP_DEC(vp9,   AV_CODEC_ID_VP9,           NULL)
RKMPP_DEC(mpeg1, AV_CODEC_ID_MPEG1VIDEO,    NULL)
RKMPP_DEC(mpeg2, AV_CODEC_ID_MPEG2VIDEO,    NULL)
RKMPP_DEC(mpeg4, AV_CODEC_ID_MPEG4,         "mpeg4_unpack_bframes")
