AC_INIT([msim], [x1.3.8.5])
AC_CONFIG_SRCDIR([src/main.c])
AM_INIT_AUTOMAKE

AC_PROG_CXX
AC_PROG_LN_S
AC_PROG_RANLIB
AC_FUNC_REALLOC
AC_PREFIX_DEFAULT(/usr/local)
AC_PROG_CC
AC_PROG_CPP
AC_PROG_CXX

LDFLAGS="$LDFLAGS -L/lib -L/usr/local/lib -L/usr/lib "
CFLAGS="$_CFLAGS -Wall"


##: ${CFLAGS=" "}
AC_PROG_CC
##if test "$GCC" = "no" ; then
CFLAGS="$CFLAGS -g -O3 -Wall -Wextra -Wno-unused-parameter -Wmissing-prototypes"
##fi

AC_CHECK_PROG(CP, cp, cp, no)
if test "$CP" = "no" ; then
	AC_MSG_ERROR(Utility cp not found (usually part of coreutils).)
fi

AC_CHECK_PROG(MV, mv, mv, no)
if test "$MV" = "no" ; then
	AC_MSG_ERROR(Utility mv not found (usually part of coreutils).)
fi

AC_CHECK_PROG(RM, rm, rm, no)
if test "$RM" = "no" ; then
	AC_MSG_ERROR(Utility rm not found (usually part of coreutils).)
fi

AC_CHECK_PROG(DIFF, diff, diff, no)
if test "$DIFF" = "no" ; then
	AC_MSG_ERROR(Utility diff not found (usually part of diffutils).)
fi

AC_CHECK_PROG(MAKEDEPEND, makedepend, makedepend, no)
if test "$MAKEDEPEND" = "no" ; then
	AC_MSG_ERROR(Utility makedepend not found (usually part of imake).)
fi

AC_PROG_INSTALL

## GNU readline
AC_CHECK_LIB(readline, rl_gnu_readline_p, [HAVE_LIBREADLINE=yes], [HAVE_LIBREADLINE=no])
 CFLAGS="$CFLAGS -I/usr/local/include" 
   CXXFLAGS="$CXXFLAGS -I/usr/local/include"
   CPPFLAGS="$CPPFLAGS -I/usr/local/include"
###if test "$HAVE_LIBREADLINE" = "no"; then
##    AC_MSG_ERROR([GNU readline is not installed.])
##fi
AC_CHECK_HEADERS(readline/readline.h, readline/rlconf.h, [HAVE_READLINE_H=yes], [HAVE_READLINE_H=no])
##if test "$HAVE_READLINE_H" = "no"; then
##    AC_MSG_ERROR([GNU readline development header is not installed.])
##fi

AC_CHECK_LIB(wsock32, main)

AC_HEADER_STDC

AC_CHECK_HEADERS([ \
	ctype.h \
	errno.h \
	fcntl.h \
	getopt.h \
	inttypes.h \
	readline/history.h \
	readline/readline.h \
	signal.h \
	stdarg.h \
	stdbool.h \
	stdint.h \
	stdio.h \
	stdlib.h \
	string.h \
	sys/stat.h \
	sys/time.h \
	sys/types.h \
	time.h \
	unistd.h \
],, [AC_MSG_FAILURE(Standard header not found.)])

AC_C_BIGENDIAN
AC_C_CONST
AC_TYPE_SIZE_T

AC_CHECK_FUNCS([getopt_long],, [AC_MSG_FAILURE(Function getopt_long not defined.)])
AC_CHECK_FUNCS([gettimeofday])
AC_CHECK_FUNCS([memset])
AC_CHECK_FUNCS([munmap])
AC_CHECK_FUNCS([select])
AC_CHECK_FUNCS([socket])
AC_CHECK_FUNCS([strchr]) 
AC_CHECK_FUNCS([strdup])
AC_CHECK_FUNCS([strerror])
AC_CHECK_FUNCS([strtol])
AC_CHECK_HEADERS([netinet/in.h])

AC_CHECK_HEADERS([stddef.h])
AC_CHECK_HEADERS([sys/socket.h]) 
AC_CHECK_HEADERS([termios.h])
AC_CHECK_HEADER_STDBOOL 
AC_C_INLINE 
AC_FUNC_ERROR_AT_LINE 
AC_FUNC_MALLOC 
AC_FUNC_MMAP 
AC_TYPE_INT16_T 
AC_TYPE_INT32_T 
AC_TYPE_INT64_T
AC_TYPE_OFF_T 
AC_TYPE_SSIZE_T 
AC_TYPE_UINT16_T 
AC_TYPE_UINT32_T 
AC_TYPE_UINT64_T 
AC_TYPE_UINT8_T 


AC_SYS_LARGEFILE

AC_SUBST(BUILDLIBS)
AC_SUBST(AFTERBUILD)
AC_SUBST(INSTALL_PREFIX)

AM_CONFIG_HEADER(config.h)
AC_CONFIG_FILES([Makefile src/Makefile])

AC_OUTPUT
