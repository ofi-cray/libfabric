dnl Configury specific to the libfabrics GNI provider

dnl Called to configure this provider
dnl
dnl Arguments:
dnl
dnl $1: action if configured successfully
dnl $2: action if not configured successfully
dnl
AC_DEFUN([FI_GNI_CONFIGURE],[
	# Determine if we can support the gni provider
        # have to pull in pkg.m4 manually
m4_include([config/pkg.m4])
	ugni_lib_happy=0
	gni_header_happy=0
	AS_IF([test x"$enable_gni" != x"no"],
	      [PKG_CHECK_MODULES([CRAY_UGNI], [cray-ugni],
                                 [ugni_lib_happy=1
                                  CPPFLAGS="$CRAY_UGNI_CFLAGS $CPPFLAGS"
                                  LDFLAGS="$CRAY_UGNI_LIBS $LDFLAGS"
                                 ],
                                 [ugni_lib_happy=0])
               PKG_CHECK_MODULES([CRAY_GNI_HEADERS], [cray-gni-headers],
                                 [gni_header_happy=1
                                  CPPFLAGS="$CRAY_GNI_HEADERS_CFLAGS $CPPFLAGS"
                                  LDFLAGS="$CRAY_GNI_HEADER_LIBS $LDFLAGS"
                                 ],
                                 [gni_header_happy=0])
               PKG_CHECK_MODULES([CRAY_ALPS_LLI], [cray-alpslli],
                                 [alps_lli_happy=1
                                  CPPFLAGS="$CRAY_ALPS_LLI_CFLAGS $CPPFLAGS"
                                  LDFLAGS="$CRAY_ALPS_LLI_LIBS $LDFLAGS"
                                 ],
                                 [alps_lli_happy=0])
               PKG_CHECK_MODULES([CRAY_ALPS_UTIL], [cray-alpsutil],
                                 [alps_util_happy=1
                                  CPPFLAGS="$CRAY_ALPS_UTIL_CFLAGS $CPPFLAGS"
                                  LDFLAGS="$CRAY_ALPS_UTIL_LIBS $LDFLAGS"
                                 ],
                                 [alps_util_happy=0])
	       ])
	AS_IF([test $gni_header_happy -eq 1 -a $ugni_lib_happy -eq 1 \
               -a $alps_lli_happy -eq 1 -a $alps_util_happy -eq 1], [$1], [$2])
])

