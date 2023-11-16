#ifndef AVRKMPP_VERSION_H
#define AVRKMPP_VERSION_H

/**
 * @file
 * Libavrkmpp version macros
 */

#include "libavutil/version.h"

#include "version_major.h"

#define LIBAVRKMPP_VERSION_MINOR 1
#define LIBAVRKMPP_VERSION_MICRO 0

#define LIBAVRKMPP_VERSION_INT AV_VERSION_INT(LIBAVRKMPP_VERSION_MAJOR, \
                                               LIBAVRKMPP_VERSION_MINOR, \
                                               LIBAVRKMPP_VERSION_MICRO)
#define LIBAVRKMPP_VERSION     AV_VERSION(LIBAVRKMPP_VERSION_MAJOR, \
                                           LIBAVRKMPP_VERSION_MINOR, \
                                           LIBAVRKMPP_VERSION_MICRO)
#define LIBAVRKMPP_BUILD       LIBAVRKMPP_VERSION_INT

#define LIBAVRKMPP_IDENT       "avrkmpp" AV_STRINGIFY(LIBAVRKMPP_VERSION)

#endif /* AVRKMPP_VERSION_H */
