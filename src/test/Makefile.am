AM_CFLAGS = \
	$(PULSEAUDIO_CFLAGS) \
	-I$(top_srcdir)/src/test \
	-I$(top_srcdir)/src/common/include/meego

AM_LIBADD = $(PULSEAUDIO_LIBS) -lm

modlibexec_LTLIBRARIES = module-meego-test.la

###################################
#              EAP                #
###################################

module_meego_test_la_SOURCES = module-meego-test.c

module_meego_test_la_LDFLAGS = -module -avoid-version -Wl,-no-undefined
module_meego_test_la_LIBADD = $(top_builddir)/src/common/libmeego-common.la $(AM_LIBADD) -lpulsecore-15.0 -lpulsecommon-15.0 -L/usr/lib/pulseaudio
module_meego_test_la_CFLAGS = $(AM_CFLAGS) -DPA_MODULE_NAME=module_meego_test

