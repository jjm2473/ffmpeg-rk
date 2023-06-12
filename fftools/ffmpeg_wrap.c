#ifdef HAVE_AV_CONFIG_H
// ffmpeg make will go here
#include "config.h"

#include <time.h>
#include <errno.h>

// for ffmpeg build
#include "cmdutils.h"
const char program_name[] = "ffmpeg_wrap";
const int program_birth_year = 2022;
void show_help_default(const char *opt, const char *arg) {}

#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>

#define ACTION_MASK 0x0FF00
#define ARG_MASK 0x00FF

typedef enum {
    CONTINUE=0x0000,
    DROP=0x0100,
    EXTEND=0x0200,
    BREAK=0x0400,
} ACTION;

const char* BARGS[]={"-x264opts:0", "-pix_fmt", "-preset", "-crf", NULL};
const char* BFLAGS[]={NULL};

static int arg_filter (const char **arg) {
    for(const char** p=BARGS;*p != NULL;++p) {
        if (!strcmp(*p, *arg)) {
            return DROP|0x2;
        }
    }
    return CONTINUE;
}

static int flag_filter (const char **arg) {
    for(const char** p=BFLAGS;*p != NULL;++p) {
        if (!strcmp(*p, *arg)) {
            return DROP|0x1;
        }
    }
    return CONTINUE;
}

char scale_rga[32] = {0};
int libx264_to_mpp = 0;
int has_input = 0;
int skip_video = 0;
int image_dump = 0;
int h264_image_dump = 0;
int dump_attachment = 0;
int copy_video = 0;
int copy_audio = 0;
int aac = 0;
int hls = 0;
int tonemap = 0;
int thumbnail = 0;
int ext_c;
const char* ext_v[16];

static char* bufprintf(const char* fmt, ...) {
    static char buf[512] = {0};
    static char *p = buf;
    int count;
    char *ret = p;
    va_list va;
    va_start(va, fmt);
    count = vsprintf(p, fmt, va);
    va_end(va);
    if (count > 0) {
        p += (count + 1);
    } else if (count < 0){
        return NULL;
    } else {
        *p = '\0';
    }
    return ret;
}

static int conv(const char **arg) {
    int w,h;
    const char *p = NULL;
    if (!strcmp("-vf", *arg) || !strcmp("-filter_complex", *arg)) {
#if CONFIG_SCALE_RGA_FILTER
        if (!strcmp("-vf", *arg)) {
            if (strstr(arg[1], "tonemap=")) {
                tonemap = 1;
            }
            if (strstr(arg[1], "thumbnail=")) {
                thumbnail = 1;
            }
        }
        p = strstr(arg[1], "scale=trunc(");

        if (p) {
            if (1 != sscanf(p, "scale=trunc(min(max(iw\\,ih*dar)\\,%d)/2)*2:trunc(ow/dar/2)*2", &w) &&
                1 != sscanf(p, "scale=trunc(min(max(iw,ih*dar),%d)/2)*2:trunc(ow/dar/2)*2", &w) &&
                1 != sscanf(p, "scale=trunc(min(max(iw\\,ih*a)\\,%d)/2)*2:trunc(ow/a/2)*2", &w) &&
                1 != sscanf(p, "scale=trunc(min(max(iw,ih*a),%d)/2)*2:trunc(ow/a/2)*2", &w) &&
                1 != sscanf(p, "scale=trunc(min(max(iw\\,ih*dar)\\,min(%d\\,", &w) &&
                1 != sscanf(p, "scale=trunc(min(max(iw,ih*dar),min(%d,", &w)) {
                w = 1920;
            }
            if (w > 1920) {
                w = 1920;
            } else if (w < 256) {
                w = 256;
            } else {
                w = w / 2 * 2;
            }
            h = w * 9 / 32 * 2;
            sprintf(scale_rga, "scale_rga=%dx%d", w, h);
            return DROP|0x2;
        }
#endif
        return DROP|0x2;
    } else if (!strncmp("-codec:v:", *arg, 9)) {
        if (!strcmp("libx264", arg[1])) {
            libx264_to_mpp = 1;
            ext_c = 0;
#if CONFIG_SCALE_RGA_FILTER
            if ('\0' == scale_rga[0])
                strcpy(scale_rga, "scale_rga=1920x1080");
            ext_v[ext_c++] = "-vf";
            ext_v[ext_c++] = scale_rga;
#endif
            ext_v[ext_c++] = *arg;
            ext_v[ext_c++] = "h264_rkmpp";
            ext_v[ext_c++] = "-flags:v";
            ext_v[ext_c++] = "-global_header";
            return EXTEND|DROP|0x2;
        } else if (!strcmp("h264", arg[1])) {
            h264_image_dump = 1;
            return DROP|0x2;
        } else if (!strcmp("copy", arg[1])) {
            copy_video = 1;
        }
#if CONFIG_LIBFDK_AAC_ENCODER
    } else if (!strcmp("-codec:a:0", *arg)) {
        if (!strcmp("aac", arg[1]) || !strcmp("libfdk_aac", arg[1])) {
            aac = 1;
            ext_c = 0;
            ext_v[ext_c++] = "-codec:a:0";
            ext_v[ext_c++] = "libfdk_aac";
            if (hls) {
                ext_v[ext_c++] = "-flags:a";
                ext_v[ext_c++] = "-global_header";
            }
            return EXTEND|DROP|0x2;
        }
#endif
    } else if (has_input && !strcmp("-f", *arg)) {
        if (!strcmp("hls", arg[1])) {
            hls = 1;
            if (aac) {
                ext_c = 2;
                ext_v[0] = "-flags:a";
                ext_v[1] = "-global_header";
                return EXTEND;
            }
        }
#if CONFIG_SCALE_RGA_FILTER
        else if (!strcmp("image2", arg[1])) {
            ext_c = 2;
            ext_v[0] = "-vf";
            ext_v[1] = "scale_rga=1280x720,hwdownload,scale";
            return EXTEND;
        }
#endif
    } else if (!strcmp("-maxrate", *arg)) {
        if (libx264_to_mpp) {
            h = atoi(arg[1]);
            ext_c = 2;
            ext_v[0] = "-b:v";
            if (h < 2000000)
                ext_v[1] = arg[1];
            else
                ext_v[1] = bufprintf("%d", h - 640000);
            return EXTEND|CONTINUE;
        }
    } else if (!strcmp("-vframes", *arg)) {
        if (!strcmp("1", arg[1])) {
            image_dump = 1;
        }
    } else if (!strcmp("-i", *arg)) {
        has_input = 1;
    } else if (!strncmp("-dump_attachment:", *arg, 17)) {
        dump_attachment = 1;
        return BREAK;
    } else if (!strcmp("-vn", *arg)) {
        skip_video = 1;
    } else if (!strcmp("-acodec", *arg)) {
        if (!strcmp("copy", arg[1])) {
            copy_audio = 1;
        }
    } else if (!strcmp("-fflags", *arg)) {
        if (!strcmp("+igndts+genpts", arg[1])) {
            return DROP|0x2;
        }
    } else if (!strcmp("-analyzeduration", *arg)) {
        ext_c = 0;
        ext_v[ext_c++] = "-analyzeduration";
        ext_v[ext_c++] = "10000000";
        ext_v[ext_c++] = "-probesize";
        ext_v[ext_c++] = "10000000";
        return EXTEND|DROP|0x2;
    }
    return CONTINUE;
}

typedef int (*Filter)(const char **) ;

Filter filters[]={conv, arg_filter, flag_filter, NULL};

#define MAX_MPP_DEC_ARGC 0

static int conv_opts(int argc, const char **argv, const char* nargv[]) {
    int nargc = MAX_MPP_DEC_ARGC+1;
    for (int i=1; i<argc; ++i) {
        Filter *f;
        for (f = filters; *f != NULL; ++f) {
            int r = (*f)(argv+i);
            if (BREAK == r) {
                return MAX_MPP_DEC_ARGC+1;
            }
            if (EXTEND == (r & EXTEND)) {
                for (int j=0; j<ext_c; ++j) {
                    nargv[nargc++] = ext_v[j];
                }
            }
            if (DROP == (r & DROP)) {
                i += (r & ARG_MASK) - 1;
                break;
            }
        }

        if (*f == NULL) {
            nargv[nargc++] = argv[i];
        }
    }
    return nargc;
}

int main(int argc, const char **argv)
{
    const char* nargv[128];
    int pargc = 0;
    int nargc = conv_opts(argc, argv, nargv);

    if (!dump_attachment) {
        nargv[nargc] = NULL;
        nargv[MAX_MPP_DEC_ARGC - pargc] = "ffmpeg";
        argv = nargv+(MAX_MPP_DEC_ARGC - pargc);
    }
    const char **p = argv;
    fprintf(stderr, "\n%s", *p);
    for (++p; *p != NULL; ++p) {
        fprintf(stderr, " \"%s\"", *p);
    }
    fprintf(stderr, "\n");
#ifndef HAVE_AV_CONFIG_H
    return 0;
#else
    return execvp("ffmpeg.mpp", argv);
#endif
}
