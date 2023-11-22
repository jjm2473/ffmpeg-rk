#include "libavutil/hwcontext_drm.h"
#include "rkformat.h"

int rkmpp_map_frame(AVFrame *frame, const rkformat *fmt, int fd, size_t size, int pitch0, int vh, void (*free)(void *opaque, uint8_t *data), void *opaque) {
    int ret;
    int uvh;
    AVDRMFrameDescriptor *desc = NULL;
    AVDRMLayerDescriptor *layer = NULL;
    frame->format           = AV_PIX_FMT_DRM_PRIME;

    desc = av_mallocz(sizeof(AVDRMFrameDescriptor));
    if (!desc) {
        return AVERROR(ENOMEM);
    }

    desc->nb_objects = 1;
    desc->objects[0].fd = fd;
    desc->objects[0].size = size;

    desc->nb_layers = 1;
    layer = &desc->layers[0];
    layer->format = fmt->drm;
    layer->nb_planes = 1;

    layer->planes[0].object_index = 0;
    layer->planes[0].offset = 0;
    layer->planes[0].pitch = pitch0;

    layer->planes[1].object_index = 0;
    layer->planes[1].offset = layer->planes[0].pitch * vh;
    layer->planes[1].pitch = pitch0;
    uvh = vh;
    switch (fmt->rga)
    {
    case RK_FORMAT_YCbCr_420_SP_10B:
    case RK_FORMAT_YCbCr_422_SP:
    case RK_FORMAT_YCbCr_420_SP:
        layer->nb_planes = 2;
        break;
    case RK_FORMAT_YCbCr_420_P:
        uvh = (vh + 1) >> 1;
        // fallthrough
    case RK_FORMAT_YCbCr_422_P:
        layer->planes[1].pitch = (pitch0 + 1) >> 1;
        layer->nb_planes = 3;
        layer->planes[2].object_index = 0;
        layer->planes[2].offset = layer->planes[1].offset + layer->planes[1].pitch * uvh;
        layer->planes[2].pitch = layer->planes[1].pitch;
        break;
    default:
        break;
    }

    frame->data[0]  = (uint8_t *)desc;
    frame->buf[0]   = av_buffer_create((uint8_t *)desc, sizeof(*desc), free,
                                       opaque, AV_BUFFER_FLAG_READONLY);

    if (!frame->buf[0]) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    return 0;

fail:
    av_free(desc);
    return ret;
}