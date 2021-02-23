SUBDIRS = tests

AM_CFLAGS = $(PULSEAUDIO_CFLAGS) $(ASOUNDLIB_CFLAGS) -I$(top_srcdir)/src/common/include/meego

AM_LIBADD = $(PULSEAUDIO_LIBS) $(ASOUNDLIB_LIBS)

modlibexec_LTLIBRARIES = module-meego-sidetone.la

module_meego_sidetone_la_SOURCES = \
	module-meego-sidetone.c \
	sidetone.c sidetone.h \
	ctrl-element.c ctrl-element.h \
	alsa-utils.c alsa-utils.h \
	alsa-util-old.c alsa-util-old.h \
	sidetone-args.c sidetone-args.h

module_meego_sidetone_la_LDFLAGS = -module -avoid-version -Wl,-no-undefined
module_meego_sidetone_la_LIBADD = $(top_builddir)/src/common/libmeego-common.la $(AM_LIBADD)
module_meego_sidetone_la_CFLAGS = $(AM_CFLAGS) -DPA_MODULE_NAME=module_meego_sidetone
