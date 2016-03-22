AM_CFLAGS = \
	$(PULSEAUDIO_CFLAGS) \
	$(DBUS_CFLAGS) \
	-DPULSEAUDIO_VERSION=@PA_MAJOR@ \
	-I$(top_srcdir)/src/common/include/meego

AM_LIBADD = $(PULSEAUDIO_LIBS) $(DBUS_LIBS)

modlibexec_LTLIBRARIES = module-stream-restore-nemo.la

noinst_HEADERS = module-stream-restore-nemo-symdef.h

module_stream_restore_nemo_la_SOURCES = module-stream-restore-nemo.c
module_stream_restore_nemo_la_LDFLAGS = -module -avoid-version -Wl,-no-undefined
module_stream_restore_nemo_la_CFLAGS = $(AM_CFLAGS)
module_stream_restore_nemo_la_LIBADD = $(AM_LIBADD) -L$(libdir)/pulse-$(PA_MAJORMINOR)/modules -lprotocol-native $(top_builddir)/src/common/libmeego-common.la
