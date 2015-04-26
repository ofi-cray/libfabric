# Makefile.am for gni provider

if HAVE_GNI

#
# want to keep ccan in the include path seen in the
# source code, so just add prov/gni to the include path
# rather than prov/gni/ccan
#
AM_CFLAGS += -I$(top_srcdir)/prov/gni/include -I$(top_srcdir)/prov/gni

_gni_files = \
	prov/gni/src/gnix_init.c \
	prov/gni/src/gnix_fabric.c \
	prov/gni/src/gnix_dom.c \
	prov/gni/src/gnix_queue.c \
	prov/gni/src/gnix_ep.c \
	prov/gni/src/gnix_cq.c \
	prov/gni/src/gnix_av.c \
	prov/gni/src/gnix_eq.c \
	prov/gni/src/gnix_mr.c \
	prov/gni/src/gnix_cm.c \
	prov/gni/src/gnix_wait.c \
	prov/gni/src/gnix_ep_rdm.c \
	prov/gni/src/gnix_nameserver.c \
	prov/gni/src/gnix_datagram.c \
	prov/gni/src/gnix_cm_nic.c \
	prov/gni/src/gnix_nic.c \
	prov/gni/src/gnix_util.c \
	prov/gni/src/gnix_bitmap.c

if HAVE_CRITERION
bin_PROGRAMS = gnitest
gnitest_SOURCES = \
	prov/gni/test/cq.c \
	prov/gni/test/utils.c \
	prov/gni/test/wait.c \
	prov/gni/test/datagram.c \
	prov/gni/test/bitmap.c

gnitest_LDFLAGS = -static
gnitest_CPPFLAGS = $(AM_CPPFLAGS)
gnitest_LDADD = -lcriterion \
		$(linkback)
endif

if HAVE_GNI_DL
pkglib_LTLIBRARIES += libgnix-fi.la
libgnix_fi_la_SOURCES = $(_gni_files) $(common_srcs)
libgnix_fi_la_LIBADD = $(linkback)
libgnix_fi_la_LDFLAGS = -module -avoid-version -shared -export-dynamic
libgnix_fi_la_DEPENDENCIES = $(linkback)
else !HAVE_GNI_DL
src_libfabric_la_SOURCES += $(_gni_files)
endif !HAVE_GNI_DL
endif
