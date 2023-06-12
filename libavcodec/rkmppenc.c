/*
 * RockChip MPP Video Encoder
 * Copyright (c) 2018 hertz.wang@rock-chips.com
 * Copyright (c) 2022 jjm2473 at gmail.com
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

#include "avcodec.h"
#include "hwconfig.h"
#include "internal.h"
#include "libavutil/avassert.h"
#include "libavutil/buffer.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/log.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"

#define SEND_FRAME_TIMEOUT          100
#define RECEIVE_PACKET_TIMEOUT      100

#define SZ_1K                   (1024)

typedef struct {
    MppCtx ctx;
    MppApi *mpi;

    char eos_reached;
} RKMPPEncoder;

typedef struct {
    AVClass *av_class;
    AVBufferRef *encoder_ref;
    MppEncPrepCfg prep_cfg;
    int profile;
    int dct8x8;
} RKMPPEncodeContext;

typedef struct {
    MppPacket packet;
    AVBufferRef *encoder_ref;
} RKMPPPacketContext;

static MppCodingType rkmpp_get_codingtype(AVCodecContext *avctx)
{
    switch (avctx->codec_id) {
    case AV_CODEC_ID_H264:          return MPP_VIDEO_CodingAVC;
    case AV_CODEC_ID_HEVC:          return MPP_VIDEO_CodingHEVC;
    case AV_CODEC_ID_VP8:           return MPP_VIDEO_CodingVP8;
    default:                        return MPP_VIDEO_CodingUnused;
    }
}

static MppFrameFormat rkmpp_get_mppformat(enum AVPixelFormat avformat)
{
    switch (avformat) {
    case AV_PIX_FMT_NV12:           return MPP_FMT_YUV420SP;
    case AV_PIX_FMT_YUV420P:        return MPP_FMT_YUV420P;
    case AV_PIX_FMT_YUYV422:        return MPP_FMT_YUV422_YUYV;
    case AV_PIX_FMT_UYVY422:        return MPP_FMT_YUV422_UYVY;
    case AV_PIX_FMT_P010:           return MPP_FMT_YUV420SP_10BIT;
    case AV_PIX_FMT_NV16:           return MPP_FMT_YUV422SP;
    default:                        return -1;
    }
}

static int rkmpp_close_encoder(AVCodecContext *avctx)
{
    RKMPPEncodeContext *rk_context = avctx->priv_data;
    av_buffer_unref(&rk_context->encoder_ref);
    return 0;
}

static void rkmpp_release_encoder(void *opaque, uint8_t *data)
{
    RKMPPEncoder *encoder = (RKMPPEncoder *)data;

    if (encoder->mpi) {
        encoder->mpi->reset(encoder->ctx);
        mpp_destroy(encoder->ctx);
        encoder->ctx = NULL;
    }

    av_free(encoder);
}

static int rkmpp_preg_config(AVCodecContext *avctx, RKMPPEncoder *encoder,
                             MppEncPrepCfg *prep_cfg)
{
    int ret;

    memset(prep_cfg, 0, sizeof(*prep_cfg));
    prep_cfg->change        = MPP_ENC_PREP_CFG_CHANGE_INPUT |
                              MPP_ENC_PREP_CFG_CHANGE_ROTATION |
                              MPP_ENC_PREP_CFG_CHANGE_FORMAT;
    prep_cfg->width         = avctx->width;
    prep_cfg->height        = avctx->height;
    prep_cfg->hor_stride    = FFALIGN(avctx->width, 16);
    prep_cfg->ver_stride    = FFALIGN(avctx->height, 16);
    prep_cfg->format        = rkmpp_get_mppformat(
        avctx->hw_frames_ctx
        ?((AVHWFramesContext *)avctx->hw_frames_ctx->data)->sw_format
        :avctx->pix_fmt);
    prep_cfg->rotation      = MPP_ENC_ROT_0;

    ret = encoder->mpi->control(encoder->ctx, MPP_ENC_SET_PREP_CFG, prep_cfg);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set prep cfg on MPI (code = %d).\n", ret);
        return AVERROR_UNKNOWN;
    }

    return 0;
}

static int rkmpp_rc_config(AVCodecContext *avctx, RKMPPEncoder *encoder,
                           MppEncRcCfg *rc_cfg)
{
    int ret;

    memset(rc_cfg, 0, sizeof(*rc_cfg));
    rc_cfg->change  = MPP_ENC_RC_CFG_CHANGE_ALL;

    rc_cfg->rc_mode = MPP_ENC_RC_MODE_VBR;
    rc_cfg->quality = MPP_ENC_RC_QUALITY_MEDIUM;
    if (avctx->flags & AV_CODEC_FLAG_QSCALE)
        rc_cfg->rc_mode = MPP_ENC_RC_MODE_FIXQP;
    else if (avctx->bit_rate > 0 && avctx->rc_max_rate == avctx->bit_rate)
        rc_cfg->rc_mode = MPP_ENC_RC_MODE_CBR;
    else if (avctx->bit_rate > 0)
        rc_cfg->rc_mode = MPP_ENC_RC_MODE_AVBR;

    rc_cfg->bps_target  = avctx->bit_rate;

    if (rc_cfg->rc_mode == MPP_ENC_RC_MODE_FIXQP) {
        rc_cfg->qp_init = 10 + avctx->global_quality / (FF_QP2LAMBDA << 2);
        if (rc_cfg->qp_init > 40)
            rc_cfg->qp_init = 40;
        av_log(avctx, AV_LOG_VERBOSE, "FIXQP %d => %d.\n", avctx->global_quality, rc_cfg->qp_init);
        rc_cfg->qp_max      = rc_cfg->qp_init;
        rc_cfg->qp_min      = rc_cfg->qp_init;
        rc_cfg->qp_max_i    = rc_cfg->qp_init;
        rc_cfg->qp_min_i    = rc_cfg->qp_init;
        rc_cfg->qp_delta_ip = 0;
    } else {
        if (rc_cfg->rc_mode == MPP_ENC_RC_MODE_VBR && rc_cfg->quality == MPP_ENC_RC_QUALITY_CQP) {
            /* constant QP does not have bps */
            rc_cfg->bps_target  = -1;
            rc_cfg->bps_max     = -1;
            rc_cfg->bps_min     = -1;
        } else {
            if (rc_cfg->rc_mode == MPP_ENC_RC_MODE_CBR) {
                av_log(avctx, AV_LOG_VERBOSE, "CBR %d bps.\n", rc_cfg->bps_target);
                /* constant bitrate has very small bps range of 1/16 bps */
                rc_cfg->bps_max     = avctx->bit_rate * 17 / 16;
                rc_cfg->bps_min     = avctx->bit_rate * 15 / 16;
            } else {
                av_log(avctx, AV_LOG_VERBOSE, "(A)VBR %d bps.\n", rc_cfg->bps_target);
                /* variable bitrate has large bps range */
                rc_cfg->bps_max     = avctx->bit_rate * 17 / 16;
                rc_cfg->bps_min     = avctx->bit_rate * 1 / 16;
            }
            rc_cfg->qp_init     = -1;
            rc_cfg->qp_max      = 51;
            rc_cfg->qp_min      = 10;
            rc_cfg->qp_max_i    = 51;
            rc_cfg->qp_min_i    = 10;
            rc_cfg->qp_delta_ip = 2;
        }
    }

    /* fix input / output frame rate */
    rc_cfg->fps_in_flex     = 0;
    av_reduce(&rc_cfg->fps_in_num, &rc_cfg->fps_in_denorm,
                avctx->time_base.den, avctx->time_base.num, 65535);

    rc_cfg->fps_out_flex    = 0;
    if (avctx->framerate.num > 0 && avctx->framerate.den > 0)
        av_reduce(&rc_cfg->fps_out_num, &rc_cfg->fps_out_denorm,
                  avctx->framerate.num, avctx->framerate.den, 65535);
    else
        av_reduce(&rc_cfg->fps_out_num, &rc_cfg->fps_out_denorm,
                  avctx->time_base.den, avctx->time_base.num, 65535);

    rc_cfg->gop             = FFMAX(avctx->gop_size, 1);
    rc_cfg->skip_cnt        = 0;

    ret = encoder->mpi->control(encoder->ctx, MPP_ENC_SET_RC_CFG, rc_cfg);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set rc cfg on MPI (code = %d).\n", ret);
        return AVERROR_UNKNOWN;
    }

    return 0;
}

static int rkmpp_codec_config(AVCodecContext *avctx, RKMPPEncoder *encoder,
                              MppCodingType codectype, RKMPPEncodeContext *ctx,
                              MppEncCodecCfg *codec_cfg)
{
    int ret;

    memset(codec_cfg, 0, sizeof(*codec_cfg));
    codec_cfg->coding = codectype;
    switch (codectype) {
    case MPP_VIDEO_CodingAVC: {
        codec_cfg->h264.change = MPP_ENC_H264_CFG_CHANGE_PROFILE |
                                 MPP_ENC_H264_CFG_CHANGE_ENTROPY |
                                 MPP_ENC_H264_CFG_CHANGE_TRANS_8x8;
        if (ctx->profile == -1) {
            ctx->profile = avctx->profile;
        }
        /*
         * H.264 profile_idc parameter
         * Support: Baseline profile
         *          Main profile
         *          High profile
         */
        if (ctx->profile != FF_PROFILE_H264_BASELINE &&
            ctx->profile != FF_PROFILE_H264_MAIN &&
            ctx->profile != FF_PROFILE_H264_HIGH) {
            av_log(avctx, AV_LOG_INFO, "Unsupport profile %d, force set to %d\n",
                   ctx->profile, FF_PROFILE_H264_HIGH);
            ctx->profile = FF_PROFILE_H264_HIGH;
        }
        codec_cfg->h264.profile = ctx->profile;

        /*
         * H.264 level_idc parameter
         * 10 / 11 / 12 / 13    - qcif@15fps / cif@7.5fps / cif@15fps / cif@30fps
         * 20 / 21 / 22         - cif@30fps / half-D1@@25fps / D1@12.5fps
         * 30 / 31 / 32         - D1@25fps / 720p@30fps / 720p@60fps
         * 40 / 41 / 42         - 1080p@30fps / 1080p@30fps / 1080p@60fps
         * 50 / 51 / 52         - 4K@30fps
         */
        if (avctx->level == FF_LEVEL_UNKNOWN) {
            av_log(avctx, AV_LOG_INFO, "Unsupport level %d, force set to %d\n",
                   avctx->level, 51);
            avctx->level = 51;
        }
        codec_cfg->h264.level               = avctx->level;
        codec_cfg->h264.entropy_coding_mode =
            (codec_cfg->h264.profile == FF_PROFILE_H264_HIGH) ? 1 : 0;
        codec_cfg->h264.cabac_init_idc      = 0;
        codec_cfg->h264.transform8x8_mode   = ctx->dct8x8==0?0:1;
    } break;
    case MPP_VIDEO_CodingMJPEG:
        codec_cfg->jpeg.change  = MPP_ENC_JPEG_CFG_CHANGE_QP;
        codec_cfg->jpeg.quant   = 10; // 1 ~ 10
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "TODO encoder coding type %d\n", codectype);
        return AVERROR_UNKNOWN;
    }

    ret = encoder->mpi->control(encoder->ctx, MPP_ENC_SET_CODEC_CFG, codec_cfg);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set codec cfg on MPI (code = %d).\n", ret);
        return AVERROR_UNKNOWN;
    }

    return 0;
}

static int rkmpp_init_encoder(AVCodecContext *avctx)
{
    int ret;
    MppCodingType codectype;
    RKMPPEncodeContext *rk_context;
    RKMPPEncoder *encoder;
    MppEncPrepCfg *prep_cfg;
    MppEncRcCfg rc_cfg;
    MppEncCodecCfg codec_cfg;
    RK_S64 paramS64;
    MppEncSeiMode sei_mode;
    RK_U8 enc_hdr_buf[SZ_1K];
    RK_S32 enc_hdr_buf_size = SZ_1K;
    MppPacket packet = NULL;

    if (avctx->hw_frames_ctx)
        av_log(avctx, AV_LOG_VERBOSE, "hw_frames_ctx->data=%p sw_format=%d(%s)\n", avctx->hw_frames_ctx->data,
            ((AVHWFramesContext*)avctx->hw_frames_ctx->data)->sw_format,
            av_get_pix_fmt_name(((AVHWFramesContext*)avctx->hw_frames_ctx->data)->sw_format));

    rk_context = avctx->priv_data;
    rk_context->encoder_ref = NULL;
    prep_cfg = &rk_context->prep_cfg;
    codectype = rkmpp_get_codingtype(avctx);
    if (codectype == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unsupport codec type (%d).\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_check_support_format(MPP_CTX_ENC, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Codec type (%d) unsupported by MPP\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // create a encoder and a ref to it
    encoder = av_mallocz(sizeof(RKMPPEncoder));
    if (!encoder) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    rk_context->encoder_ref =
        av_buffer_create((uint8_t *)encoder, sizeof(*encoder),
                         rkmpp_release_encoder, NULL, AV_BUFFER_FLAG_READONLY);
    if (!rk_context->encoder_ref) {
        av_free(encoder);
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    av_log(avctx, AV_LOG_DEBUG, "Initializing RKMPP encoder.\n");

    // mpp init
    ret = mpp_create(&encoder->ctx, &encoder->mpi);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_init(encoder->ctx, MPP_CTX_ENC, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // mpp setup
    ret = rkmpp_preg_config(avctx, encoder, prep_cfg);
    if (ret)
        goto fail;

    ret = rkmpp_rc_config(avctx, encoder, &rc_cfg);
    if (ret)
        goto fail;

    ret = rkmpp_codec_config(avctx, encoder, codectype, rk_context, &codec_cfg);
    if (ret)
        goto fail;

    sei_mode = MPP_ENC_SEI_MODE_ONE_FRAME;
    ret = encoder->mpi->control(encoder->ctx, MPP_ENC_SET_SEI_CFG, &sei_mode);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set sei cfg on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // TODO: osd if hardware support

    paramS64 = SEND_FRAME_TIMEOUT;
    ret = encoder->mpi->control(encoder->ctx, MPP_SET_INPUT_TIMEOUT, &paramS64);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set input timeout on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    paramS64 = RECEIVE_PACKET_TIMEOUT;
    ret = encoder->mpi->control(encoder->ctx, MPP_SET_OUTPUT_TIMEOUT, &paramS64);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set output timeout on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    memset(enc_hdr_buf, 0 , enc_hdr_buf_size);
    ret = mpp_packet_init(&packet, (void *)enc_hdr_buf, enc_hdr_buf_size);
    if (!packet) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init extra info packet (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    mpp_packet_set_length(packet, 0);
    ret = encoder->mpi->control(encoder->ctx, MPP_ENC_GET_HDR_SYNC, packet);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get extra info on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    /* get and write sps/pps for H.264 */
    void *ptr   = mpp_packet_get_pos(packet);
    size_t len  = mpp_packet_get_length(packet);

    if (avctx->extradata != NULL && avctx->extradata_size != len) {
        av_free(avctx->extradata);
        avctx->extradata = NULL;
    }
    if (!avctx->extradata)
        avctx->extradata = av_malloc(len);
    if (avctx->extradata == NULL) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    avctx->extradata_size = len;
    memcpy(avctx->extradata, ptr, len);

    packet = NULL;

    av_log(avctx, AV_LOG_DEBUG, "RKMPP encoder initialized successfully.\n");

    return 0;

fail:
    av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP encoder.\n");
    rkmpp_close_encoder(avctx);
    return ret;
}

static int rkmpp_queue_frame(AVCodecContext *avctx, RKMPPEncoder *encoder, MppEncPrepCfg *prep_cfg,
                             const AVFrame *avframe, MppFrame *out_frame)
{
    int ret;
    MppFrameFormat mppformat;
    MppCtx ctx;
    MppApi *mpi;
    MppBuffer buffer = NULL;
    MppBufferInfo info;
    MppFrame frame = NULL;
    MppTask task = NULL;
    RK_S32 hor_stride;
    RK_S32 ver_stride;

    // check format
    if (avframe) {
        enum AVPixelFormat swformat;
        if(avframe->format != AV_PIX_FMT_DRM_PRIME) {
            av_log(avctx, AV_LOG_ERROR, "RKMPPEncoder only support fmt DRM\n");
            return AVERROR(EINVAL);
        }
        swformat    = ((AVHWFramesContext*)avframe->hw_frames_ctx->data)->sw_format;
        av_log(avctx, AV_LOG_DEBUG, "hw_frames_ctx->data=%p sw_format=%d\n", avframe->hw_frames_ctx->data,
            ((AVHWFramesContext*)avframe->hw_frames_ctx->data)->sw_format);
        mppformat   = rkmpp_get_mppformat(swformat);
        if (mppformat < 0) {
            av_log(avctx, AV_LOG_ERROR, "Unsupport av format %d\n", swformat);
            return AVERROR(EINVAL);
        }
    }

    ret = mpp_frame_init(&frame);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed init mpp frame on encoder (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }
    mpp_frame_set_eos(frame, encoder->eos_reached);

    if (avframe) {
        AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor*)avframe->data[0];
        AVDRMLayerDescriptor *layer = &desc->layers[0];

        mpp_frame_set_pts(frame, avframe->pts);
        mpp_frame_set_dts(frame, avframe->pkt_dts);
        mpp_frame_set_width(frame, avframe->width);
        mpp_frame_set_height(frame, avframe->height);
        if (mppformat == MPP_FMT_YUV422_YUYV || mppformat == MPP_FMT_YUV422_UYVY) {
            hor_stride = 2 * layer->planes[0].pitch;
        } else {
            // nv12 or yuv420p
            hor_stride = layer->planes[0].pitch;
        }
        if (layer->nb_planes > 1)
            ver_stride = layer->planes[1].offset / layer->planes[0].pitch;
        else
            ver_stride = avframe->height;

        if (prep_cfg->hor_stride != hor_stride || prep_cfg->ver_stride != ver_stride) {
            prep_cfg->change = MPP_ENC_PREP_CFG_CHANGE_INPUT;
            prep_cfg->hor_stride = hor_stride;
            prep_cfg->ver_stride = ver_stride;
            ret = encoder->mpi->control(encoder->ctx, MPP_ENC_SET_PREP_CFG, prep_cfg);
            if (ret != MPP_OK) {
                av_log(avctx, AV_LOG_ERROR, "Failed to set prep cfg on MPI (code = %d).\n", ret);
                return AVERROR_UNKNOWN;
            }
        }

        mpp_frame_set_hor_stride(frame, hor_stride/* /bpp */);
        mpp_frame_set_ver_stride(frame, ver_stride);
        mpp_frame_set_fmt(frame, mppformat);

        memset(&info, 0, sizeof(info));
        info.type   = MPP_BUFFER_TYPE_DRM;
        info.size   = desc->objects[0].size;
        info.fd     = desc->objects[0].fd;
        ret = mpp_buffer_import(&buffer, &info);
        if (ret != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to import buffer\n");
            ret = AVERROR(EINVAL);
            goto out;
        }
        mpp_frame_set_buffer(frame, buffer);
    }

    ctx = encoder->ctx;
    mpi = encoder->mpi;
    ret = mpi->poll(ctx, MPP_PORT_INPUT, MPP_POLL_BLOCK);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to poll task input (ret = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto out;
    }

    ret = mpi->dequeue(ctx, MPP_PORT_INPUT, &task);
    if (ret != MPP_OK || NULL == task) {
        av_log(avctx, AV_LOG_ERROR, "Failed to dequeue task input (ret = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto out;
    }

    mpp_task_meta_set_frame (task, KEY_INPUT_FRAME, frame);
    ret = mpi->enqueue(ctx, MPP_PORT_INPUT, task);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to enqueue task input (ret = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto out;
    }
    *out_frame = frame;
    frame = NULL;

out:
    if (buffer)
        mpp_buffer_put(buffer);
    if (frame)
        mpp_frame_deinit(&frame);
    return 0;
}

static int rkmpp_send_frame(AVCodecContext *avctx, const AVFrame *frame,
                            MppFrame *mpp_frame)
{
    int ret;
    RKMPPEncodeContext *rk_context = avctx->priv_data;
    RKMPPEncoder *encoder = (RKMPPEncoder *)rk_context->encoder_ref->data;

    if (!frame) {
        av_log(avctx, AV_LOG_DEBUG, "End of stream.\n");
        encoder->eos_reached = 1;
        ret = rkmpp_queue_frame(avctx, encoder, NULL, NULL, mpp_frame);
        if (ret)
            av_log(avctx, AV_LOG_ERROR, "Failed to send EOS to encoder (code = %d)\n", ret);
        return ret;
    }

    if (frame->pict_type == AV_PICTURE_TYPE_I) {
        ret = encoder->mpi->control(encoder->ctx, MPP_ENC_SET_IDR_FRAME, NULL);
        if (ret != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set IDR frame on MPI (code = %d).\n", ret);
            return AVERROR_UNKNOWN;
        }
    }
    ret = rkmpp_queue_frame(avctx, encoder, &rk_context->prep_cfg, frame, mpp_frame);
    if (ret && ret != AVERROR(EAGAIN))
        av_log(avctx, AV_LOG_ERROR, "Failed to send frame to encoder (code = %d)\n", ret);

    return ret;
}

static void rkmpp_release_packet(void *opaque, uint8_t *data)
{
    RKMPPPacketContext *pkt_ctx = (RKMPPPacketContext *)opaque;

    mpp_packet_deinit(&pkt_ctx->packet);
    av_buffer_unref(&pkt_ctx->encoder_ref);
    av_free(pkt_ctx);
}

static int rkmpp_receive_packet(AVCodecContext *avctx, AVPacket *pkt,
                                MppFrame *mpp_frame)
{
    int ret;
    RKMPPEncodeContext *rk_context = avctx->priv_data;
    RKMPPEncoder *encoder = (RKMPPEncoder *)rk_context->encoder_ref->data;
    MppCtx ctx = encoder->ctx;
    MppApi *mpi = encoder->mpi;
    MppTask task = NULL;
    MppPacket packet = NULL;

    ret = mpi->poll(ctx, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed to poll task output (ret = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpi->dequeue(ctx, MPP_PORT_OUTPUT, &task);
    if (ret || NULL == task) {
        av_log(avctx, AV_LOG_ERROR, "Failed to dequeue task output (ret = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    if (task) {
        mpp_task_meta_get_packet(task, KEY_OUTPUT_PACKET, &packet);
        ret = mpi->enqueue(ctx, MPP_PORT_OUTPUT, task);
        if (ret != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to enqueue task output (ret = %d)\n", ret);
            ret = AVERROR_UNKNOWN;
            goto fail;
        }
    }

    if (packet) {
        RKMPPPacketContext *pkt_ctx;
        MppMeta meta = NULL;
        int keyframe = 0;

        if (mpp_packet_get_eos(packet)) {
            av_log(avctx, AV_LOG_DEBUG, "Received a EOS packet.\n");
            if (encoder->eos_reached) {
                ret = AVERROR_EOF;
                goto fail;
            }
        }
        pkt_ctx = av_mallocz(sizeof(*pkt_ctx));
        if (!pkt_ctx) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        pkt_ctx->packet = packet;
        pkt_ctx->encoder_ref = av_buffer_ref(rk_context->encoder_ref);

        // TODO: outside need fd from mppbuffer?
        pkt->data = mpp_packet_get_data(packet);
        pkt->size = mpp_packet_get_length(packet);
        pkt->buf = av_buffer_create((uint8_t*)pkt->data, pkt->size,
            rkmpp_release_packet, pkt_ctx, AV_BUFFER_FLAG_READONLY);
        if (!pkt->buf) {
            av_buffer_unref(&pkt_ctx->encoder_ref);
            av_free(pkt_ctx);
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        pkt->pts = mpp_packet_get_pts(packet);
        pkt->dts = mpp_packet_get_dts(packet);
        if (pkt->pts <= 0)
            pkt->pts = pkt->dts;
        if (pkt->dts <= 0)
            pkt->dts = pkt->pts;
        meta = mpp_packet_get_meta(packet);
        if (meta)
            mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &keyframe);
        if (keyframe)
            pkt->flags |= AV_PKT_FLAG_KEY;

        packet = NULL;
    }

fail:
    if (packet)
        mpp_packet_deinit(&packet);
    if (*mpp_frame) {
        mpp_frame_deinit(mpp_frame);
        *mpp_frame = NULL;
    }
    return ret;
}

static int rkmpp_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                              const AVFrame *frame, int *got_packet)
{
    int ret;
    MppFrame mpp_frame = NULL;

    ret = rkmpp_send_frame(avctx, frame, &mpp_frame);
    if (ret)
        return ret;

    ret = rkmpp_receive_packet(avctx, pkt, &mpp_frame);
    av_assert0(mpp_frame == NULL);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        *got_packet = 0;
    } else if (ret) {
        return ret;
    } else {
        *got_packet = 1;
    }

    return 0;
}

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
    AVCodec ff_##NAME##_rkmpp_encoder = { \
        .name           = #NAME "_rkmpp", \
        .long_name      = NULL_IF_CONFIG_SMALL(#NAME " (rkmpp)"), \
        .type           = AVMEDIA_TYPE_VIDEO, \
        .id             = ID, \
        .init           = rkmpp_init_encoder, \
        .close          = rkmpp_close_encoder, \
        .encode2        = rkmpp_encode_frame, \
        .priv_data_size = sizeof(RKMPPEncodeContext), \
        .priv_class     = &rkmpp_##NAME##_enc_class, \
        .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE, \
        .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP, \
        .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_DRM_PRIME, \
                                                         AV_PIX_FMT_NONE }, \
        .hw_configs     = rkmpp_hw_configs, \
        .bsfs           = BSFS, \
        .wrapper_name   = "rkmpp", \
    };

RKMPP_ENC(h264, AV_CODEC_ID_H264, NULL)
