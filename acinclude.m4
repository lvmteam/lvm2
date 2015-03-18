dnl AC_GCC_VERSION
dnl check for compiler version
dnl sets COMPILER_VERSION and GCC_VERSION

AC_DEFUN([AC_CC_VERSION],
[
    AC_MSG_CHECKING([C compiler version])
    COMPILER_VERSION=`$CC -v 2>&1 | grep version`
    case "$COMPILER_VERSION" in
        *gcc*)
	   dnl Ok, how to turn $3 into the real $3
	   GCC_VERSION=`echo $COMPILER_VERSION | \
	   sed -e 's/[[^ ]]*\ [[^ ]]*\ \([[^ ]]*\)\ .*/\1/'` ;;
	*) GCC_VERSION=unknown ;;
    esac
    AC_MSG_RESULT($GCC_VERSION)
])

dnl AC_TRY_CCFLAG([CCFLAG], [VAR], [ACTION-IF-WORKS], [ACTION-IF-FAILS])
dnl check if $CC supports a given flag

AC_DEFUN([AC_TRY_CCFLAG],
[
    AC_REQUIRE([AC_PROG_CC])
    ac_save_CFLAGS=$CFLAGS
    CFLAGS=$1
    AC_CACHE_CHECK([whether $CC accepts $1 flag], [ac_cv_flag_$2],
	[AC_COMPILE_IFELSE([AC_LANG_PROGRAM()],
			   [AS_VAR_SET([ac_cv_flag_$2], [yes])],
			   [AS_VAR_SET([ac_cv_flag_$2], [no])])])
    CFLAGS=$ac_save_CFLAGS
    $2=AS_VAR_GET([ac_cv_flag_$2])
    if test "$2" = yes; then
        ifelse([$3], [], [:], [$3])
    else
        ifelse([$4], [], [:], [$4])
    fi
])

dnl AC_IF_YES([TEST-FOR-YES], [ACTION-IF-TRUE], [ACTION-IF-FALSE])
dnl AS_IF() abstraction, checks shell variable for 'yes'
AC_DEFUN([AC_IF_YES], [AS_IF([test $$1 = yes], [$2], [$3])])

dnl AC_TRY_LDFLAGS([LDFLAGS], [VAR], [ACTION-IF-WORKS], [ACTION-IF-FAILS])
dnl check if $CC supports given ld flags

AC_DEFUN([AC_TRY_LDFLAGS],
[
    AC_REQUIRE([AC_PROG_CC])
    ac_save_LDFLAGS=$LDFLAGS
    LDFLAGS=$1
	AC_CACHE_CHECK([whether $CC accepts $1 ld flags], [ac_cv_flag_$2],
	[AC_LINK_IFELSE([AC_LANG_PROGRAM()],
			[AS_VAR_SET([ac_cv_flag_$2], [yes])],
			[AS_VAR_SET([ac_cv_flag_$2], [no])])])
    LDFLAGS=$ac_save_LDFLAGS
    $2=AS_VAR_GET([ac_cv_flag_$2])
    if test "$2" = yes; then
        ifelse([$3], [], [:], [$3])
    else
        ifelse([$4], [], [:], [$4])
    fi
])
