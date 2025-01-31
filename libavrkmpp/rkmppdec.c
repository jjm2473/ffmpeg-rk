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
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#include "avrkmpp.h"
#include "rkmpp.h"

#include "libavutil/hwcontext_drm.h"

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
    const rkformat *fmt;

    char print_fps;

    uint64_t last_fps_time;
    uint64_t frames;

    char sync;

    // mjpeg only
    int8_t mjpeg;
    int jpeg_frame_buf_size;
    MppPacket eos_packet;

} RKMPPDecoder;

typedef struct {
    MppFrame frame;
    AVBufferRef *decoder_ref;
} RKMPPFrameContext;

int avrkmpp_close_decoder(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;

    av_packet_unref(&decoder->packet);

    av_buffer_unref(&rk_context->decoder_ref);
    return 0;
}

static const rkformat *rkmpp_get_format(MppFrameFormat mppformat) {
    return rkmpp_get_mpp_format(mppformat & MPP_FRAME_FMT_MASK);
}

static void rkmpp_release_decoder(void *opaque, uint8_t *data)
{
    RKMPPDecoder *decoder = (RKMPPDecoder *)data;

    if (decoder->eos_packet) {
        mpp_packet_deinit(&decoder->eos_packet);
        decoder->eos_packet = NULL;
    }
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

    // HACK: somehow MPP cannot handle extra data for AV1
    if (avctx->extradata_size && avctx->codec_id != AV_CODEC_ID_AV1) {
        ret = mpp_packet_init(&packet, avctx->extradata, avctx->extradata_size);
        if (ret < 0)
            return AVERROR_UNKNOWN;
        ret = decoder->mpi->decode_put_packet(decoder->ctx, packet);
        mpp_packet_deinit(&packet);
        if (ret < 0)
            return AVERROR_UNKNOWN;
    }

    return 0;
}

static int mpp_packet_create_with_buffer(MppPacket *pkt, MppBufferGroup frame_group, void *data, size_t size) {
    int ret;
    MppBuffer buffer;
    MppPacket newpkt;
    if (ret = mpp_buffer_get(frame_group, &buffer, size))
        return ret;
    memcpy(mpp_buffer_get_ptr(buffer), data, size);
    ret = mpp_packet_init_with_buffer(&newpkt, buffer);
    mpp_buffer_put(buffer);
    if (ret)
        return ret;
    *pkt = newpkt;
    return 0;
}

int avrkmpp_init_decoder(AVCodecContext *avctx)
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

    codectype = rkmpp_get_codingtype(avctx->codec_id);
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

    ret = mpp_buffer_group_get_internal(&decoder->frame_group, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_DMA32);
    if (ret) {
       av_log(avctx, AV_LOG_ERROR, "Failed to get buffer group (code = %d)\n", ret);
       ret = AVERROR_UNKNOWN;
       goto fail;
    }

    if (MPP_VIDEO_CodingMJPEG == codectype) {
        if (avctx->width <= 0 || avctx->height <= 0) {
            av_log(avctx, AV_LOG_ERROR, "width and height must be specified on mjpeg mode\n");
            ret = AVERROR(EINVAL);
            goto fail;
        }
        decoder->mjpeg = 1;
        decoder->jpeg_frame_buf_size = FFALIGN(avctx->width, 16) * FFALIGN(avctx->height, 16) * 2;

        ret = mpp_packet_create_with_buffer(&decoder->eos_packet, decoder->frame_group, (void*)"", 1);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "Failed to init EOS packet (code = %d)\n", ret);
            ret = AVERROR_UNKNOWN;
            goto fail;
        }

        mpp_packet_set_size(decoder->eos_packet, 0);
        mpp_packet_set_length(decoder->eos_packet, 0);
        mpp_packet_set_eos(decoder->eos_packet);
    } else {
        ret = decoder->mpi->control(decoder->ctx, MPP_DEC_SET_EXT_BUF_GROUP, decoder->frame_group);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "Failed to assign buffer group (code = %d)\n", ret);
            ret = AVERROR_UNKNOWN;
            goto fail;
        }
    }

    decoder->mpi->control(decoder->ctx, MPP_DEC_SET_DISABLE_ERROR, NULL);

    // wait for decode result after feeding any packets
    if (getenv("FFMPEG_RKMPP_SYNC")){
        decoder->sync = 1;
        ret = 1;
        decoder->mpi->control(decoder->ctx, MPP_DEC_SET_IMMEDIATE_OUT, &ret);
    }
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

    decoder->fmt = NULL;
    av_log(avctx, AV_LOG_DEBUG, "RKMPP decoder initialized successfully.\n");

    return 0;

fail:
    av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP decoder.\n");
    avrkmpp_close_decoder(avctx);
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

static int rkmpp_get_frame_mjpeg(RKMPPDecoder *decoder, int timeout, MppFrame *mppframe) {
    MppPacket mpkt = NULL;
    MppTask mtask = NULL;
    MppFrame mframe = NULL;
    MppMeta meta;
    int ret;

    if (ret = decoder->mpi->poll(decoder->ctx, MPP_PORT_OUTPUT, timeout))
        return timeout==MPP_POLL_BLOCK?ret:MPP_ERR_TIMEOUT;

    decoder->mpi->dequeue(decoder->ctx, MPP_PORT_OUTPUT, &mtask);
    if (!mtask)
        return MPP_ERR_TIMEOUT;

    mpp_task_meta_get_frame (mtask, KEY_OUTPUT_FRAME, &mframe);
    if (!mframe) {
        ret = MPP_ERR_TIMEOUT;
        goto done;
    }

    meta = mpp_frame_get_meta (mframe);
    mpp_meta_get_packet (meta, KEY_INPUT_PACKET, &mpkt);
    if (mpkt)
        mpp_packet_deinit (&mpkt);

    *mppframe = mframe;
    ret = MPP_OK;

done:
    decoder->mpi->enqueue(decoder->ctx, MPP_PORT_OUTPUT, mtask);
    return ret;
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
    int mode;

    // should not provide any frame after EOS
    if (decoder->eos)
        return AVERROR_EOF;

    if (decoder->mjpeg) {
        ret = rkmpp_get_frame_mjpeg(decoder, timeout==MPP_TIMEOUT_BLOCK?200:timeout, &mppframe);
    } else {
        decoder->mpi->control(decoder->ctx, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&timeout);

        ret = decoder->mpi->decode_get_frame(decoder->ctx, &mppframe);
    }

    if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get frame (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    if (!mppframe) {
        if (timeout != MPP_TIMEOUT_NON_BLOCK)
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

    if (mpp_frame_get_info_change(mppframe) || (decoder->mjpeg && !decoder->frames_ref)) {
        AVHWFramesContext *hwframes;
        MppFrameFormat mppformat;
        const rkformat *rkformat;

        av_log(avctx, AV_LOG_INFO, "Decoder noticed an info change (%dx%d), stride(%dx%d), format=0x%x\n",
               (int)mpp_frame_get_width(mppframe), (int)mpp_frame_get_height(mppframe),
               (int)mpp_frame_get_hor_stride(mppframe), (int)mpp_frame_get_ver_stride(mppframe), 
               (int)mpp_frame_get_fmt(mppframe));

        avctx->width = mpp_frame_get_width(mppframe);
        avctx->height = mpp_frame_get_height(mppframe);

        // chromium would align planes' width and height to 32, adding this
        // hack to avoid breaking the plane buffers' contiguous.
        avctx->coded_width = FFALIGN(avctx->width, 64);
        avctx->coded_height = FFALIGN(avctx->height, 64);

        if (!decoder->mjpeg)
            decoder->mpi->control(decoder->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

        av_buffer_unref(&decoder->frames_ref);

        decoder->frames_ref = av_hwframe_ctx_alloc(decoder->device_ref);
        if (!decoder->frames_ref) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        av_log(avctx, AV_LOG_VERBOSE, "hw_frames_ctx->data=%p\n", decoder->frames_ref->data);

        mppformat = mpp_frame_get_fmt(mppframe);
        rkformat = rkmpp_get_format(mppformat);
        if (!rkformat) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported RKMPP frame format %x.\n", mppformat);
            ret = AVERROR_UNKNOWN;
            goto fail;
        }

        hwframes = (AVHWFramesContext*)decoder->frames_ref->data;
        hwframes->format    = AV_PIX_FMT_DRM_PRIME;
        hwframes->sw_format = rkformat->av;
        hwframes->width     = rkformat->mpp==MPP_FMT_YUV420SP_10BIT?mpp_frame_get_hor_stride(mppframe):avctx->width;
        hwframes->height    = mpp_frame_get_ver_stride(mppframe);
        ret = av_hwframe_ctx_init(decoder->frames_ref);
        if (!ret) {
            decoder->fmt = rkformat;
            ret = AVERROR(EAGAIN);
        }
        av_buffer_unref(&avctx->hw_frames_ctx);
        avctx->hw_frames_ctx = av_buffer_ref(decoder->frames_ref);

        if (!decoder->mjpeg)
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

    // setup general frame fields
    frame->width            = mpp_frame_get_width(mppframe);
    frame->height           = mpp_frame_get_height(mppframe);
    frame->pts              = mpp_frame_get_pts(mppframe);
    frame->reordered_opaque = frame->pts;
    frame->color_range      = mpp_frame_get_color_range(mppframe);
    frame->color_primaries  = mpp_frame_get_color_primaries(mppframe);
    frame->color_trc        = mpp_frame_get_color_trc(mppframe);
    frame->colorspace       = mpp_frame_get_colorspace(mppframe);

    mode = mpp_frame_get_mode(mppframe);
    frame->interlaced_frame = ((mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) == MPP_FRAME_FLAG_DEINTERLACED);
    frame->top_field_first  = ((mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) == MPP_FRAME_FLAG_TOP_FIRST);

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

    ret = rkmpp_map_frame(frame, decoder->fmt, mpp_buffer_get_fd(buffer), mpp_buffer_get_size(buffer),
        mpp_frame_get_hor_stride(mppframe), mpp_frame_get_ver_stride(mppframe),
        rkmpp_release_frame, framecontextref);
    if (ret)
        goto fail;

    frame->hw_frames_ctx = av_buffer_ref(decoder->frames_ref);
    if (!frame->hw_frames_ctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    return 0;

fail:
    if (mppframe)
        mpp_frame_deinit(&mppframe);

    if (framecontext)
        av_buffer_unref(&framecontext->decoder_ref);

    if (framecontextref)
        av_buffer_unref(&framecontextref);

    return ret;
}

static int rkmpp_send_packet_mjpeg(RKMPPDecoder *decoder, MppPacket mpkt, int eos) {
    int ret;
    MppBuffer buffer;
    MppFrame mframe = NULL;
    MppTask mtask = NULL;
    MppMeta meta;
    MppPacket newpkt = NULL;

    decoder->mpi->poll(decoder->ctx, MPP_PORT_INPUT, eos?MPP_POLL_BLOCK:5);
    decoder->mpi->dequeue(decoder->ctx, MPP_PORT_INPUT, &mtask);
    if (!mtask) {
        ret = MPP_ERR_TIMEOUT;
        goto error;
    }

    mpp_frame_init (&mframe);

    if (!eos) {
        if (NULL == mpp_packet_get_buffer(mpkt)) {
            ret = mpp_packet_create_with_buffer(&newpkt, decoder->frame_group,
                mpp_packet_get_data(mpkt), mpp_packet_get_size(mpkt));
            if (ret)
                goto error;
            mpp_packet_set_pts(newpkt, mpp_packet_get_pts(mpkt));
            mpp_packet_deinit(&mpkt);
            mpkt = newpkt;
        }

        if (ret = mpp_buffer_get(decoder->frame_group, &buffer, decoder->jpeg_frame_buf_size))
            goto error;

        mpp_frame_set_buffer (mframe, buffer);
        mpp_buffer_put (buffer);
        meta = mpp_frame_get_meta (mframe);
        mpp_meta_set_packet (meta, KEY_INPUT_PACKET, mpkt);
    }

    mpp_task_meta_set_packet(mtask, KEY_INPUT_PACKET, mpkt);

    mpp_task_meta_set_frame (mtask, KEY_OUTPUT_FRAME, mframe);

    if (ret = decoder->mpi->enqueue(decoder->ctx, MPP_PORT_INPUT, mtask))
        goto error;

    return 0;
error:
    if (mtask) {
        mpp_task_meta_set_packet (mtask, KEY_INPUT_PACKET, NULL);
        mpp_task_meta_set_frame (mtask, KEY_OUTPUT_FRAME, NULL);
        decoder->mpi->enqueue(decoder->ctx, MPP_PORT_INPUT, mtask);
    }

    if (mframe)
        mpp_frame_deinit (&mframe);

    mpp_packet_deinit(&mpkt);
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

    if (decoder->mjpeg) {
        ret = rkmpp_send_packet_mjpeg(decoder, mpkt, 0);
    } else {
        ret = decoder->mpi->decode_put_packet(decoder->ctx, mpkt);
        mpp_packet_deinit(&mpkt);
    }

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

    if (decoder->mjpeg) {
        ret = rkmpp_send_packet_mjpeg(decoder, decoder->eos_packet, 1);
    } else {
        mpp_packet_init(&mpkt, NULL, 0);
        mpp_packet_set_eos(mpkt);
        do {
            ret = decoder->mpi->decode_put_packet(decoder->ctx, mpkt);
        } while (ret != MPP_OK);
        mpp_packet_deinit(&mpkt);
    }

    decoder->draining = 1;

    return 0;
}

int avrkmpp_receive_frame(AVCodecContext *avctx, AVFrame *frame, 
    int (*ff_decode_get_packet)(AVCodecContext *, AVPacket *))
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

void avrkmpp_decoder_flush(AVCodecContext *avctx)
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
