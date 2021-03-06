dnl These options seem to only be used for the 2.4.x
dnl Linux kernel build
AC_DEFUN([OPENAFS_GCC_SUPPORTS_MARCH], [
  AC_CACHE_CHECK([if $CC accepts -march=pentium],
    [openafs_cv_gcc_supports_march],
    [save_CFLAGS="$CFLAGS"
     CFLAGS="-MARCH=pentium"
     AC_COMPILE_IFELSE(
       [AC_LANG_PROGRAM(
         [[]],
         [[int x;]])],
         [openafs_cv_gcc_supports_march=yes],
         [openafs_cv_gcc_supports_march=no])
     CFLAGS="$save_CFLAGS"
    ])
  AS_IF([test x$openafs_cv_gcc_supports_march = xyes],
        [P5PLUS_KOPTS="-march=pentium"],
        [P5PLUS_KOPTS="-m486 -malign-loops=2 -malign-jumps=2 -malign-functions=2"])
])

AC_DEFUN([OPENAFS_GCC_NEEDS_NO_STRICT_ALIASING], [
  AC_CACHE_CHECK([if $CC needs -fno-strict-aliasing],
    [openafs_cv_gcc_needs_no_strict_aliasing],
    [save_CFLAGS="$CFLAGS"
     CFLAGS="-fno-strict-aliasing"
     AC_COMPILE_IFELSE(
       [AC_LANG_PROGRAM(
         [[]],
         [[int x;]])],
       [openafs_cv_gcc_needs_no_strict_aliasing=yes],
       [openafs_cv_gcc_needs_no_strict_aliasing=no])
     CFLAGS="$save_CFLAGS"
  ])
  AS_IF([test x$openafs_cv_gcc_needs_no_strict_aliasing = xyes],
        [LINUX_GCC_KOPTS="$LINUX_GCC_KOPTS -fno-strict-aliasing"])
])

AC_DEFUN([OPENAFS_GCC_NEEDS_NO_STRENGTH_REDUCE], [
  AC_CACHE_CHECK([if $CC needs -fno-strength-reduce],
    [openafs_cv_gcc_needs_no_strength_reduce],
    [save_CFLAGS="$CFLAGS"
     CFLAGS="-fno-strength-reduce"
     AC_COMPILE_IFELSE(
       [AC_LANG_PROGRAM(
         [[]],
         [[int x;]])],
       [openafs_cv_gcc_needs_no_strength_reduce=yes],
       [openafs_cv_gcc_needs_no_strength_reduce=no])
     CFLAGS="$save_CFLAGS"
  ])
  AS_IF([test x$openafs_cv_gcc_needs_no_strength_reduce = xyes],
        [LINUX_GCC_KOPTS="$LINUX_GCC_KOPTS -fno-strength-reduce"])
])

AC_DEFUN([OPENAFS_GCC_SUPPORTS_NO_COMMON], [
  AC_CACHE_CHECK([if $CC supports -fno-common],
    [openafs_cv_gcc_supports_no_common],
    [save_CFLAGS="$CFLAGS"
     CFLAGS="-fno-common"
     AC_COMPILE_IFELSE(
       [AC_LANG_PROGRAM(
         [[]],
         [[int x;]])],
       [openafs_cv_gcc_supports_no_common=yes],
       [openafs_cv_gcc_supports_no_common=no])

     CFLAGS="$save_CFLAGS"
  ])
  AS_IF([test x$openafs_cv_gcc_supports_no_common = xyes],
	[LINUX_GCC_KOPTS="$LINUX_GCC_KOPTS -fno-common"])
])

AC_DEFUN([OPENAFS_GCC_SUPPORTS_PIPE], [
  AC_CACHE_CHECK([if $CC supports -pipe],
    [openafs_cv_gcc_supports_pipe],
    [save_CFLAGS="$CFLAGS"
     CFLAGS="-pipe"
     AC_COMPILE_IFELSE(
       [AC_LANG_PROGRAM(
         [[]],
         [[int x;]])],
       [openafs_cv_gcc_supports_pipe=yes],
       [openafs_cv_gcc_supports_pipe=no])
  CFLAGS="$save_CFLAGS"
  ])
  AS_IF([test x$openafs_cv_gcc_supports_pipe = xyes],
	  [LINUX_GCC_KOPTS="$LINUX_GCC_KOPTS -pipe"])
])

AC_DEFUN([OPENAFS_GCC_SUPPORTS_WNO_ERROR_FRAME_LARGER_THAN], [
  AC_CACHE_CHECK([if $CC supports -Wno-error=frame-larger-than=],
    [openafs_cv_gcc_supports_wno_error_frame_larger_than],
    [save_CFLAGS="$CFLAGS"
     CFLAGS="-Wno-error=frame-larger-than="
     AC_COMPILE_IFELSE(
       [AC_LANG_PROGRAM(
         [[]],
         [[int x;]])],
       [openafs_cv_gcc_supports_wno_error_frame_larger_than=yes],
       [openafs_cv_gcc_supports_wno_error_frame_larger_than=no])
  CFLAGS="$save_CFLAGS"
  ])
  AS_IF([test x$openafs_cv_gcc_supports_wno_error_frame_larger_than = xyes && test x"$CFLAGS_NOERROR" != x],
        dnl Only use -Wno-error=frame-larger-than= if gcc supports it, and if
        dnl CFLAGS_NOERROR is nonempty (so if --enable-checking=all is used, we
        dnl don't pass -Wno-error=frame-larger-than=)
	[CFLAGS_WNOERROR_FRAME_LARGER_THAN="-Wno-error=frame-larger-than="])
  AC_SUBST([CFLAGS_WNOERROR_FRAME_LARGER_THAN])
])
