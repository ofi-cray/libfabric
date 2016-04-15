dnl Configury specific to the libfabrics GNI provider

dnl Called to configure this provider

m4_include([config/fi_pkg.m4])

AC_DEFUN([FI_GNI_CONFIGURE],[
        # Determine if we can support the gni provider
        # have to pull in pkg.m4 manually
        ugni_lib_happy=0
        gni_header_happy=0
        alps_lli_happy=0
        alps_util_happy=0
        have_criterion=false
        criterion_tests_present=true
        gni_CPPFLAGS=
        gni_LDFLAGS=
        gni_LIBS=
	gnitest_CPPFLAGS=
	gnitest_LDFLAGS=
        gnitest_LIBS=

        AS_IF([test x"$enable_gni" != x"no"],
               [FI_PKG_CHECK_MODULES([CRAY_GNI_HEADERS], [cray-gni-headers],
                                 [gni_header_happy=1
                                  gni_CPPFLAGS="$CRAY_GNI_HEADERS_CFLAGS $gni_CPPFLAGS"
                                  gni_LDFLAGS="$CRAY_GNI_HEADER_LIBS $gni_LDFLAGS"
                                 ],
                                 [gni_header_happy=0])
              ])

        AS_IF([test "$gni_header_happy" -eq 1],
              [FI_PKG_CHECK_MODULES([CRAY_UGNI], [cray-ugni],
                                 [ugni_lib_happy=1
                                  gni_CPPFLAGS=$CRAY_UGNI_CFLAGS
                                  gni_LDFLAGS=$CRAY_UGNI_LIBS
                                 ],
                                 [ugni_lib_happy=0])
               FI_PKG_CHECK_MODULES_STATIC([CRAY_ALPS_LLI], [cray-alpslli],
                                 [alps_lli_happy=1
                                  gni_CPPFLAGS="$CRAY_ALPS_LLI_CFLAGS $gni_CPPFLAGS"
                                  gni_LDFLAGS="$CRAY_ALPS_LLI_LIBS $gni_LDFLAGS"
                                 ],
                                 [alps_lli_happy=0])
               FI_PKG_CHECK_MODULES([CRAY_ALPS_UTIL], [cray-alpsutil],
                                 [alps_util_happy=1
                                  gni_CPPFLAGS="$CRAY_ALPS_UTIL_CFLAGS $gni_CPPFLAGS"
                                  gni_LDFLAGS="$CRAY_ALPS_UTIL_LIBS $gni_LDFLAGS"
                                 ],
                                 [alps_util_happy=0])
               gni_path_to_gni_pub=${CRAY_GNI_HEADERS_CFLAGS:2}
dnl looks like we need to get rid of some white space
               gni_path_to_gni_pub=${gni_path_to_gni_pub%?}/gni_pub.h

               AC_CHECK_TYPES([gni_ct_cqw_post_descriptor_t], [],
                              [AC_MSG_WARN([GNI provider requires CLE 5.2.UP04 or higher. Disabling gni provider.])
                               gni_header_happy=0
                              ],
                              [[#include "$gni_path_to_gni_pub"]])

               AS_IF([test -d $srcdir/prov/gni/test],
                     [AC_ARG_WITH([criterion], [AS_HELP_STRING([--with-criterion],
                     [Location for criterion unit testing framework])])],
                     [criterion_tests_present=false])

               if test "$with_criterion" != "" && test "$with_criterion" != "no"; then
	       	     if test "$enable_direct" != "" && test "$enable_direct" != "no"; then
		     	gnitest_CPPFLAGS="-I$srcdir/prov/gni/include"
		     fi

                     AS_IF([test "$criterion_tests_present" = "true"],
                           [AC_MSG_CHECKING([criterion path])
                            if test -d "$with_criterion"; then
                                AC_MSG_RESULT([yes])
                                gnitest_CPPFLAGS="-I$with_criterion/include $gnitest_CPPFLAGS"
				gnitest_LIBS="-lcriterion $gnitest_LIBS"

                                if test -d "$with_criterion/lib"; then
                                        gnitest_LDFLAGS="$CRAY_ALPS_LLI_STATIC_LIBS -L$with_criterion/lib -Wl,-rpath=$with_criterion/lib $gnitest_LDFLAGS"
                                        have_criterion=true
                                elif test -d "$with_criterion/lib64"; then
                                        gnitest_LDFLAGS="$CRAY_ALPS_LLI_STATIC_LIBS -L$with_criterion/lib64 -Wl,-rpath=$with_criterion/lib64 $gnitest_LDFLAGS"
                                        have_criterion=true
                                else
                                        have_criterion=false
                                fi
                                FI_PKG_CHECK_MODULES([CRAY_PMI], [cray-pmi],
                                                     [],
                                                     [have_criterion=false])
                            else
                                AC_MSG_RESULT([no])
                                AC_MSG_ERROR([criterion requested but invalid path given])
                            fi],
                           [AC_MSG_ERROR([criterion requested tests not available])])
               fi

               AC_CHECK_DECL([HAVE_ATOMICS],
                             [],
                             [cc_version=`$CC --version | head -n1`
                              AC_MSG_WARN(["$cc_version" doesn't support native atomics.  Disabling GNI provider.])
                              ugni_lib_happy=0])

        ])

        AM_CONDITIONAL([HAVE_CRITERION], [test "x$have_criterion" = "xtrue"])

        AC_SUBST(gni_CPPFLAGS)
        AC_SUBST(gni_LDFLAGS)
        AC_SUBST(gni_LIBS)
	AC_SUBST(gnitest_CPPFLAGS)
        AC_SUBST(gnitest_LDFLAGS)
        AC_SUBST(gnitest_LIBS)

        AS_IF([test $gni_header_happy -eq 1 -a $ugni_lib_happy -eq 1 \
               -a $alps_lli_happy -eq 1 -a $alps_util_happy -eq 1], [$1], [$2])
])
