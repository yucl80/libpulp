# Include common definitions
include $(top_srcdir)/Makefile.common

# Convenience libraries
#
# Each library is built twice: with and without -Bsymbolic passed to the
# linker, to test that libpulp's ref-by-name variable access works in
# both scenarios.
check_LTLIBRARIES = libdozens.la \
                    libhundreds.la \
                    libdozens_bsymbolic.la \
                    libhundreds_bsymbolic.la
noinst_HEADERS = dozens.h hundreds.h

libdozens_la_SOURCES = dozens.c $(TARGET_TRM_SOURCES)
libdozens_la_CFLAGS = $(TARGET_CFLAGS)
libdozens_la_LDFLAGS = $(TARGET_LDFLAGS) $(CONVENIENCE_LDFLAGS)

libhundreds_la_SOURCES = hundreds.c $(TARGET_TRM_SOURCES)
libhundreds_la_CFLAGS = $(TARGET_CFLAGS)
libhundreds_la_LDFLAGS = $(TARGET_LDFLAGS) $(CONVENIENCE_LDFLAGS)

libdozens_bsymbolic_la_SOURCES = dozens.c $(TARGET_TRM_SOURCES)
libdozens_bsymbolic_la_CFLAGS = $(TARGET_CFLAGS)
libdozens_bsymbolic_la_LDFLAGS = -Wl,-Bsymbolic \
  $(TARGET_LDFLAGS) $(CONVENIENCE_LDFLAGS)

libhundreds_bsymbolic_la_SOURCES = hundreds.c $(TARGET_TRM_SOURCES)
libhundreds_bsymbolic_la_CFLAGS = $(TARGET_CFLAGS)
libhundreds_bsymbolic_la_LDFLAGS = -Wl,-Bsymbolic \
  $(TARGET_LDFLAGS) $(CONVENIENCE_LDFLAGS)

POST_PROCESS = .libs/libdozens.post \
               .libs/libhundreds.post \
               .libs/libdozens_bsymbolic.post \
               .libs/libhundreds_bsymbolic.post

# Live patches
check_LTLIBRARIES += libdozens_livepatch1.la \
                     libhundreds_livepatch1.la \
                     libdozens_bsymbolic_livepatch1.la \
                     libhundreds_bsymbolic_livepatch1.la

libdozens_livepatch1_la_SOURCES = libdozens_livepatch1.c
libdozens_livepatch1_la_LDFLAGS = $(CONVENIENCE_LDFLAGS)

libhundreds_livepatch1_la_SOURCES = libhundreds_livepatch1.c
libhundreds_livepatch1_la_LDFLAGS = $(CONVENIENCE_LDFLAGS)

libdozens_bsymbolic_livepatch1_la_SOURCES = libdozens_livepatch1.c
libdozens_bsymbolic_livepatch1_la_LDFLAGS = $(CONVENIENCE_LDFLAGS)

libhundreds_bsymbolic_livepatch1_la_SOURCES = libhundreds_livepatch1.c
libhundreds_bsymbolic_livepatch1_la_LDFLAGS = $(CONVENIENCE_LDFLAGS)

METADATA = \
  libdozens_livepatch1.dsc \
  libdozens_livepatch1.ulp \
  libdozens_livepatch1.rev \
  libhundreds_livepatch1.dsc \
  libhundreds_livepatch1.ulp \
  libhundreds_livepatch1.rev \
  libdozens_bsymbolic_livepatch1.dsc \
  libdozens_bsymbolic_livepatch1.ulp \
  libdozens_bsymbolic_livepatch1.rev \
  libhundreds_bsymbolic_livepatch1.dsc \
  libhundreds_bsymbolic_livepatch1.ulp \
  libhundreds_bsymbolic_livepatch1.rev

EXTRA_DIST = \
  libdozens_livepatch1.in \
  libhundreds_livepatch1.in \
  libdozens_bsymbolic_livepatch1.in \
  libhundreds_bsymbolic_livepatch1.in

clean-local:
	rm -f $(METADATA)

# Test programs
check_PROGRAMS = numserv numserv_bsymbolic

numserv_SOURCES = numserv.c
numserv_LDADD = libdozens.la libhundreds.la
numserv_DEPENDENCIES = $(POST_PROCESS) $(METADATA)

numserv_bsymbolic_SOURCES = numserv.c
numserv_bsymbolic_LDADD = libdozens_bsymbolic.la libhundreds_bsymbolic.la
numserv_bsymbolic_DEPENDENCIES = $(POST_PROCESS) $(METADATA)

TESTS = numserv.py numserv_bsymbolic.py
TEST_EXTENSIONS = .py
PY_LOG_COMPILER = $(PYTHON)

EXTRA_DIST += $(TESTS)