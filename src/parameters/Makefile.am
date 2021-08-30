AM_CFLAGS = $(PULSEAUDIO_CFLAGS) -I$(top_srcdir)/src -I$(top_srcdir)/src/common/include

AM_LIBADD = -lm $(top_builddir)/src/common/libmeego-common.la $(PULSEAUDIO_LIBS)

modlibexec_LTLIBRARIES = module-meego-parameters.la

module_meego_parameters_la_SOURCES = module-meego-parameters.c \
	parameters.c parameters.h

module_meego_parameters_la_LDFLAGS = -module -avoid-version -Wl,-no-undefined
module_meego_parameters_la_LIBADD = $(AM_LIBADD) -lpulsecore-15.0 -lpulsecommon-15.0 -L/usr/lib/pulseaudio
module_meego_parameters_la_CFLAGS = $(AM_CFLAGS) -DPA_MODULE_NAME=module_meego_parameters
