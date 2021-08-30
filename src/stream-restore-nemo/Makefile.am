AM_CFLAGS = \
	$(PULSEAUDIO_CFLAGS) \
	$(DBUS_CFLAGS) \
	-DHAVE_DBUS \
	-I$(top_srcdir)/src/common/include/meego

AM_LIBADD = $(PULSEAUDIO_LIBS) $(DBUS_LIBS)

modlibexec_LTLIBRARIES = module-stream-restore-nemo.la

module_stream_restore_nemo_la_SOURCES = module-stream-restore-nemo.c
module_stream_restore_nemo_la_LDFLAGS = -module -avoid-version -Wl,-no-undefined
module_stream_restore_nemo_la_CFLAGS = $(AM_CFLAGS) -DPA_MODULE_NAME=module_stream_restore_nemo
module_stream_restore_nemo_la_LIBADD = $(top_builddir)/src/common/libmeego-common.la $(AM_LIBADD) -L$(libdir)/pulse-$(PA_MAJORMINOR)/modules -lprotocol-native -lpulsecore-15.0 -lpulsecommon-15.0 -L/usr/lib/pulseaudio
