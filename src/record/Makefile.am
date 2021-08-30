AM_LIBADD =
AM_CFLAGS =						\
	$(PULSEAUDIO_CFLAGS)				\
	-I$(top_srcdir)/src/record			\
	-I$(top_srcdir)/src/common/include/meego

AM_LIBADD += $(top_builddir)/src/common/libmeego-common.la

modlibexec_LTLIBRARIES = module-meego-record.la

module_meego_record_la_SOURCES = module-meego-record.c

module_meego_record_la_LDFLAGS = -module -avoid-version -Wl,-no-undefined -Wl,-z,noexecstack
module_meego_record_la_LIBADD = $(AM_LIBADD) $(PULSEAUDIO_LIBS) -lm -lpulsecore-15.0 -lpulsecommon-15.0 -L/usr/lib/pulseaudio
module_meego_record_la_CFLAGS = $(AM_CFLAGS) -DPA_MODULE_NAME=module_meego_record
