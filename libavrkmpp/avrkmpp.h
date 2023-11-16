
#ifndef AVRKMPP_AVRKMPP_H
#define AVRKMPP_AVRKMPP_H

#include "version_major.h"
#ifndef HAVE_AV_CONFIG_H
/* When included as part of the ffmpeg build, only include the major version
 * to avoid unnecessary rebuilds. When included externally, keep including
 * the full version information. */
#include "version.h"
#endif

/**
 * Return the LIBAVRKMPP_VERSION_INT constant.
 */
unsigned avrkmpp_version(void);

/**
 * Return the libavrkmpp build-time configuration.
 */
const char *avrkmpp_configuration(void);

/**
 * Return the libavrkmpp license.
 */
const char *avrkmpp_license(void);

#include "libavutil/log.h"
#include "libavutil/buffer.h"
#include "libavutil/frame.h"

typedef struct ScaleRGAContext {
    const AVClass *class;

    AVBufferRef *filter_ref;
    int width;
    int height;

    char *w_expr;      // width expression string
    char *h_expr;      // height expression string
    char *size_str;    // WxH expression
    int force_original_aspect_ratio;
    int force_divisible_by;

    int down_scale_only;
} ScaleRGAContext;

#include "libavfilter/avfilter.h"

int avrkmpp_scale_rga_filter_frame(AVFilterLink *, AVFrame *, AVFrame **);

int avrkmpp_scale_rga_config_output(AVFilterLink *);

int avrkmpp_scale_rga_init(AVFilterContext *);

void avrkmpp_scale_rga_uninit(AVFilterContext *);


typedef struct {
    AVClass *av_class;
    AVBufferRef *decoder_ref;
} RKMPPDecodeContext;

#include "libavcodec/avcodec.h"

int avrkmpp_init_decoder(AVCodecContext *);
int avrkmpp_close_decoder(AVCodecContext *);
int avrkmpp_receive_frame(AVCodecContext *, AVFrame *, int (*)(AVCodecContext *, AVPacket *));
void avrkmpp_decoder_flush(AVCodecContext *);


typedef struct {
    AVClass *av_class;
    AVBufferRef *encoder_ref;
    int profile;
    int dct8x8;
} RKMPPEncodeContext;

int avrkmpp_init_encoder(AVCodecContext *);
int avrkmpp_close_encoder(AVCodecContext *);
int avrkmpp_encode_frame(AVCodecContext *, AVPacket *, const AVFrame *, int *);


#endif /* AVRKMPP_AVRKMPP_H */
