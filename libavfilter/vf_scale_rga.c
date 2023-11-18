/**
 * @file
 * Rockchip RGA based scale filter
 * @author: jjm2473 (jjm2473 at gmail.com)
 */

#include "libavrkmpp/avrkmpp.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "internal.h"
#include "scale_eval.h"

#include "libavutil/opt.h"

static int scale_rga_query_formats(AVFilterContext *avctx) {
    enum AVPixelFormat input_pix_fmts[] = {
        AV_PIX_FMT_DRM_PRIME,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NV12,
        // AV_PIX_FMT_P010,
        AV_PIX_FMT_NV16,
        AV_PIX_FMT_YUYV422,
        AV_PIX_FMT_UYVY422,
        AV_PIX_FMT_RGBA,
        AV_PIX_FMT_RGB0,
        AV_PIX_FMT_BGRA,
        AV_PIX_FMT_BGR0,
        AV_PIX_FMT_ARGB,
        AV_PIX_FMT_0RGB,
        AV_PIX_FMT_ABGR,
        AV_PIX_FMT_0BGR,
        AV_PIX_FMT_RGB24,
        AV_PIX_FMT_BGR24,
        AV_PIX_FMT_RGB565,
        AV_PIX_FMT_BGR565,
        AV_PIX_FMT_GRAY8,
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

    return avrkmpp_scale_rga_query_formats(avctx);
}

static int scale_rga_filter_frame_l(AVFilterLink *inlink, AVFrame *input_frame) {
    AVFrame *output_frame;
    AVFilterContext *avctx   = inlink->dst;
    AVFilterLink *outlink    = avctx->outputs[0];
    int ret = avrkmpp_scale_rga_filter_frame(inlink, input_frame, &output_frame);
    if (!ret) {
        ret = ff_filter_frame(outlink, output_frame);
    }
    return ret;
}

static int scale_rga_config_input_l(AVFilterLink *inlink) {
    int err;
    AVFilterContext *avctx = inlink->dst;
    ScaleRGAContext *ctx   = avctx->priv;
    AVFilterLink dummy_outlink = {0};
    if (ctx->pix_fmt)
        dummy_outlink.format = av_get_pix_fmt(ctx->pix_fmt);
    if (dummy_outlink.format == AV_PIX_FMT_NONE)
        dummy_outlink.format = AV_PIX_FMT_NV12;

    if ((err = ff_scale_eval_dimensions(ctx,
                                        ctx->w_expr, ctx->h_expr,
                                        inlink, &dummy_outlink,
                                        &ctx->width, &ctx->height)) < 0)
        return err;

    ff_scale_adjust_dimensions(inlink, &ctx->width, &ctx->height,
                               ctx->force_original_aspect_ratio, ctx->force_divisible_by);

    if ((ctx->down_scale_only == 1) && (ctx->width > inlink->w || ctx->height > inlink->h)) {
        ctx->width = inlink->w;
        ctx->height = inlink->h;
    }

    return avrkmpp_scale_rga_config_input(inlink);
}

static int scale_rga_config_output_l(AVFilterLink *outlink) {
    int err;
    AVFilterLink *inlink   = outlink->src->inputs[0];

    if ((err = avrkmpp_scale_rga_config_output(outlink)) < 0) {
        return err;
    }

    if (inlink->sample_aspect_ratio.num)
        outlink->sample_aspect_ratio = av_mul_q((AVRational){outlink->h * inlink->w, outlink->w * inlink->h}, inlink->sample_aspect_ratio);
    else
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    return 0;
}

static av_cold int init_dict(AVFilterContext *ctx)
{
    ScaleRGAContext *scale = ctx->priv;
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
        if ((ret = av_parse_video_size(&scale->width, &scale->height, scale->size_str)) < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "Invalid size '%s'\n", scale->size_str);
            return ret;
        }
        snprintf(buf, sizeof(buf)-1, "%d", scale->width);
        av_opt_set(scale, "w", buf, 0);
        snprintf(buf, sizeof(buf)-1, "%d", scale->height);
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

static av_cold int scale_rga_init_l(AVFilterContext *avctx) {
    int ret;
    if (ret = init_dict(avctx))
        return ret;

    return avrkmpp_scale_rga_init(avctx);
}

static av_cold void scale_rga_uninit_l(AVFilterContext *avctx) {
    avrkmpp_scale_rga_uninit(avctx);
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
    { "format", "pixel format", OFFSET(pix_fmt), AV_OPT_TYPE_STRING, .flags = FLAGS },
    { "hdr2sdr", "HDR to SDR", OFFSET(hdr2sdr), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(scale_rga);

static const AVFilterPad scale_rga_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &scale_rga_filter_frame_l,
        .config_props = &scale_rga_config_input_l,
    },
};

static const AVFilterPad scale_rga_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = &scale_rga_config_output_l,
    },
};

const AVFilter ff_vf_scale_rga = {
    .name          = "scale_rga",
    .description   = NULL_IF_CONFIG_SMALL("Scale to/from RGA surfaces."),
    .priv_size     = sizeof(ScaleRGAContext),
    .priv_class    = &scale_rga_class,
    .init          = &scale_rga_init_l,
    .uninit        = &scale_rga_uninit_l,
    FILTER_INPUTS(scale_rga_inputs),
    FILTER_OUTPUTS(scale_rga_outputs),
    FILTER_QUERY_FUNC(&scale_rga_query_formats),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
