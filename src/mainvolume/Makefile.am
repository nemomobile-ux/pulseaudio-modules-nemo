AM_CFLAGS = \
	$(PULSEAUDIO_CFLAGS) \
	$(DBUS_CFLAGS) \
	-DPULSEAUDIO_VERSION=@PA_MAJOR@ \
	-I$(top_srcdir)/src/common/include

AM_LIBADD = $(PULSEAUDIO_LIBS) $(DBUS_LIBS)

modlibexec_LTLIBRARIES = module-meego-mainvolume.la

noinst_HEADERS = module-meego-mainvolume-symdef.h

module_meego_mainvolume_la_SOURCES = mainvolume.c mainvolume.h listening-watchdog.c listening-watchdog.h module-meego-mainvolume.c

module_meego_mainvolume_la_LDFLAGS = -module -avoid-version -Wl,-no-undefined
module_meego_mainvolume_la_LIBADD = $(top_builddir)/src/common/libmeego-common.la $(AM_LIBADD)
module_meego_mainvolume_la_CFLAGS = $(AM_CFLAGS)

SUBDIRS = tests

