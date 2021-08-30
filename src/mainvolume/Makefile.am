AM_CFLAGS = \
	$(PULSEAUDIO_CFLAGS) \
	$(DBUS_CFLAGS) \
	-I$(top_srcdir)/src/common/include

AM_LIBADD = $(PULSEAUDIO_LIBS) $(DBUS_LIBS)

modlibexec_LTLIBRARIES = module-meego-mainvolume.la

module_meego_mainvolume_la_SOURCES = mainvolume.c mainvolume.h listening-watchdog.c listening-watchdog.h module-meego-mainvolume.c

module_meego_mainvolume_la_LDFLAGS = -module -avoid-version -Wl,-no-undefined
module_meego_mainvolume_la_LIBADD = $(top_builddir)/src/common/libmeego-common.la $(AM_LIBADD) -lpulsecore-15.0 -lpulsecommon-15.0 -L/usr/lib/pulseaudio
module_meego_mainvolume_la_CFLAGS = $(AM_CFLAGS) -DPA_MODULE_NAME=module_meego_mainvolume

SUBDIRS = tests

