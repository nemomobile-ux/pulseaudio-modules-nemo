AM_CFLAGS = $(PULSEAUDIO_CFLAGS) -I$(top_srcdir)/inc -I$(top_srcdir)/src

AM_LIBADD = $(PULSEAUDIO_LIBS) -lm $(top_builddir)/src/common/libmeego-common.la

modlibexec_LTLIBRARIES = module-meego-test-parameters.la

module_meego_test_parameters_la_SOURCES = module-meego-test-parameters.c

module_meego_test_parameters_la_LDFLAGS = -module -avoid-version -Wl,-no-undefined
module_meego_test_parameters_la_LIBADD = $(AM_LIBADD)
module_meego_test_parameters_la_CFLAGS = $(AM_CFLAGS) -DPA_MODULE_NAME=module_meego_test_parameters
