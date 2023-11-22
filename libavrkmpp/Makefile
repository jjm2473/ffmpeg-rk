NAME = avrkmpp
DESC = Rockchip MPP adapter

HEADERS = avrkmpp.h                                                     \
          version.h                                                     \
          version_major.h                                               \

OBJS-codec = rkmppdec.o                                                 \
             rkmppenc.o                                                 \

OBJS-filter-$(CONFIG_LIBRGA) = vf_scale_rga.o                           \

OBJS = $(OBJS-codec) $(OBJS-filter-yes) rkformat.o rkframe.o version.o

$(OBJS-codec:%=$(SUBDIR)%): CFLAGS  += -I$(SRC_LINK)/libavcodec/

$(OBJS-filter-yes:%=$(SUBDIR)%): CFLAGS  += -I$(SRC_LINK)/libavfilter/
