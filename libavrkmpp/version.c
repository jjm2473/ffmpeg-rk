#include "config.h"
#include "avrkmpp.h"
#include "version.h"

#include "libavutil/ffversion.h"
const char avrkmpp_ffversion[] = "FFmpeg version " FFMPEG_VERSION;

unsigned avrkmpp_version(void)
{
    return LIBAVRKMPP_VERSION_INT;
}

const char *avrkmpp_configuration(void)
{
    return FFMPEG_CONFIGURATION;
}

const char *avrkmpp_license(void)
{
#define LICENSE_PREFIX "libavrkmpp license: "
    return &LICENSE_PREFIX FFMPEG_LICENSE[sizeof(LICENSE_PREFIX) - 1];
}
