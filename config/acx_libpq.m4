dnl Checking for libpq and postgres backend features
dnl
AC_DEFUN([ACX_LIBPQ], [
AC_REQUIRE([AC_CANONICAL_HOST])
AC_LANG_SAVE
AC_LANG_C

dnl -----------------------------------------
dnl Get commandline options first
dnl -----------------------------------------


if test -n "${with_pgbindir}" ; then
_pwd=`pwd`
cd ${with_pgbindir}
with_pgbindir=`pwd`
cd ${_pwd}
echo "overriding pgbindir with" ${with_pgbindir}
PG_BINDIR=${with_pgbindir}
fi

if test -n "${with_pglibdir}"; then
_pwd=`pwd`
cd ${with_pglibdir}
with_pglibdir=`pwd`
cd ${_pwd}
echo "overriding pglibdir with" ${with_pglibdir}
PG_LIBDIR=${with_pglibdir}
fi

if test -n "${with_pgincludedir}"; then
_pwd=`pwd`
cd ${with_pgincludedir}
with_pgincludedir=`pwd`
cd ${_pwd}
echo "overriding pgincludedir with" ${with_pgincludedir}
PG_INCLUDEDIR=${with_pgincludedir}
fi

if test -n "${with_pgpkglibdir}"; then
_pwd=`pwd`
cd ${with_pgpkglibdir}
with_pgpkglibdir=`pwd`
cd ${_pwd}
echo "overriding pgpkglibdir with" ${with_pgpkglibdir}
PG_PKGLIBDIR=${with_pgpkglibdir}
fi

if test -n "${with_pgincludeserverdir}"; then
_pwd=`pwd`
cd ${with_pgincludeserverdir}
with_pgincludeserverdir=`pwd`
cd ${_pwd}
echo "overriding pgincludeserverdir with" ${with_pgincludeserverdir}
PG_INCLUDESERVERDIR=${with_pgincludeserverdir}
fi

if test -n "${with_toolsdir}"; then
_pwd=`pwd`
cd ${with_toolsdir}
with_toolsdir=`pwd`
cd ${_pwd}
echo "overriding toolsdir with" ${with_toolsdir}
TOOLSBINDIR=${with_toolsdir}
fi

if test -n "${with_pgsharedir}"; then
_pwd=`pwd`
cd ${with_pgsharedir} 2>/dev/null
_share=`pwd`
cd ${_pwd}
	if test ${_pwd} = ${_share}; then
		echo "overriding pgsharedir with" ${with_pgsharedir} "even though it wasn't found :)"
	else
		echo "overriding pgsharedir with" ${with_pgsharedir}
		with_pgsharedir=${_share}
	fi
PG_SHAREDIR=${with_pgsharedir}
fi


AC_MSG_CHECKING(for pg_config)

dnl Checking for pg_config in a list of possible locations.
PGCONFIG_POSSIBLE_LOCATIONS="${with_pgconfigdir} /usr/local/pgsql/bin /usr/local/bin /usr/bin /bin /usr/lib/postgresql/bin /opt/local/pgsql/bin /opt/pgsql/bin/"
for i in $PGCONFIG_POSSIBLE_LOCATIONS; do
    if test -x $i/pg_config; then
	    PG_CONFIG_LOCATION="$i/pg_config"
        WIN32_PG_LOCATION=`echo $i | sed 's/\/bin//'`
	    break;
    fi
done

if test -n "$PG_CONFIG_LOCATION"; then
    AC_MSG_RESULT([$PG_CONFIG_LOCATION])
    
    dnl
    dnl On win32 pg_config returns DOS style paths which Mingw/GCC cannot use.
    dnl Use default values in this case, rather than pg_config supplied ones.
    dnl Also note that trailing /'s seem to cause compile failures.
    dnl
    
    case "${host_os}" in
        *mingw32*)
            PG_BINDIR=$WIN32_PG_LOCATION/bin
            PG_LIBDIR=$WIN32_PG_LOCATION/lib
            PG_INCLUDEDIR=$WIN32_PG_LOCATION/include
            PG_PKGLIBDIR=$WIN32_PG_LOCATION/lib
            PG_INCLUDESERVERDIR=$WIN32_PG_LOCATION/include/server
            PG_SHAREDIR=$WIN32_PG_LOCATION/share
            ;;
        
        *)
	    if test "$PG_BINDIR" = ""; then
		PG_BINDIR=`$PG_CONFIG_LOCATION --bindir`/
		echo "pg_config says pg_bindir is $PG_BINDIR"
		fi
            if test "$PG_LIBDIR" = ""; then
        	PG_LIBDIR=`$PG_CONFIG_LOCATION --libdir`/
		echo "pg_config says pg_libdir is $PG_LIBDIR"
	    fi
            if test "$PG_INCLUDEDIR" = ""; then
        	PG_INCLUDEDIR=`$PG_CONFIG_LOCATION --includedir`/
		echo "pg_config says pg_includedir is $PG_INCLUDEDIR"
	    fi
            if test "$PG_PKGLIBDIR" = ""; then
        	PG_PKGLIBDIR=`$PG_CONFIG_LOCATION --pkglibdir`/
		echo "pg_config says pg_pkglibdir is $PG_PKGLIBDIR"
	    fi
            if test "$PG_INCLUDESERVERDIR" = ""; then
        	PG_INCLUDESERVERDIR=`$PG_CONFIG_LOCATION --includedir-server`/
		echo "pg_config says pg_includeserverdir is $PG_INCLUDESERVERDIR"
	    fi
            ;;
    esac
    
    PG_CONFIGURE=`$PG_CONFIG_LOCATION --configure`
    pg_config_version=`$PG_CONFIG_LOCATION --version`
    PG_VERSION=`expr "$pg_config_version" : '[[^0-9]]*\([[0-9]]*\.[[0-9]]*\)'`

    AC_MSG_CHECKING(for correct version of PostgreSQL)
    PG_VERSION_MAJOR=`echo $PG_VERSION | cut -d. -f1`
    PG_VERSION_MINOR=`echo $PG_VERSION | cut -d. -f2`
    if test "$PG_VERSION_MAJOR" = "7"; then
	    AC_MSG_RESULT("error")
	    AC_MSG_ERROR(Your version of PostgreSQL ($PG_VERSION) is lower 
	    than the required 8.3.  Slony-I needs functionality included in
	    a newer version.)
    fi
    if test "$PG_VERSION_MAJOR" = "8"; then
	if test $PG_VERSION_MINOR -gt 2; then
	    AC_MSG_RESULT($PG_VERSION)
	    AC_DEFINE(PG_VERSION_OK,1,[PostgreSQL 8.3 or later])
	else
	    AC_MSG_RESULT("error")
	    AC_MSG_ERROR(Your version of PostgreSQL ($PG_VERSION) is lower 
	    than the required 8.3.  Slony-I needs functionality included in
	    a newer version.)
	fi
      AC_MSG_RESULT($PG_VERSION)
      AC_DEFINE(PG_VERSION_OK,1,[PostgreSQL 8.3 or later])
    fi

    
    if test "$PG_SHAREDIR" = ""; then
       PG_SHAREDIR=`$PG_CONFIG_LOCATION --sharedir`/ 2>/dev/null
       echo "pg_config says pg_sharedir is $PG_SHAREDIR"
    fi

    case ${host_os} in
	aix*|*solaris*)
		AC_MSG_CHECKING(PostgreSQL for enable-thread-safety as required on ${host_os})
		PG_ETS=`echo $PG_CONFIGURE | grep -c enable-thread-safety`
		if test $PG_ETS -eq 0; then
			AC_MSG_RESULT(no)
			AC_MSG_ERROR(PostgreSQL needs to be compiled with --enable-thread-safety on ${host_os})
		else
			AC_MSG_RESULT(yes)
		fi
	;;
        *mingw32*)
                if test $PG_VERSION_MAJOR -ge 8 -a $PG_VERSION_MINOR -ge 2; then
                        AC_SUBST(NEED_PG_DLLINIT, 0)
                else
                        AC_SUBST(NEED_PG_DLLINIT, 1)
                fi
        ;;
	*)
	;;
    esac
	dnl ----
	dnl Define the detected PostgreSQL version
	dnl ----
	AC_DEFINE_UNQUOTED(PG_VERSION_MAJOR,$PG_VERSION_MAJOR,[PostgreSQL major version])
	AC_DEFINE_UNQUOTED(PG_VERSION_MINOR,$PG_VERSION_MINOR,[PostgreSQL minor version])
else
    dnl Specify the commandline options here.
    AC_MSG_ERROR(Cannot find pg_config. 
    Please specify correct installation path for pg_config 
    with --with-pgconfigdir=<dir>
    )
fi


dnl -----------------------------------------
dnl Make sure we have found the right values!
dnl Values that were overridden will not be
dnl checked.
dnl -----------------------------------------

dnl -----------------------------------------
dnl Because aix needs pg built with thread 
dnl saftey we force PTHREAD_LIBS to be added 
dnl to the test compile, there are more OS's
dnl that this may/should be done for
dnl


case ${host_os} in
aix*)
        LIBS="$LIBS  $PTHREAD_LIBS"
;;
esac


TEMP_LDFLAGS=$LDFLAGS
LDFLAGS="$TEMP_LDFLAGS -L$PG_LIBDIR"

AC_CHECK_LIB(pq, PQunescapeBytea, HAVE_PQUNESCAPEBYTEA=1)
if test -n "$HAVE_PQUNESCAPEBYTEA"; then
    AC_DEFINE(PG_VERSION_VERIFIED,1,[PostgreSQL 7.3 or later check])
else
    AC_MSG_ERROR(Your version of libpq doesn't have PQunescapeBytea
     which means that your version of PostgreSQL is lower than 7.3
     and thus not even remotely supported by Slony-I version 2, 
     which requires 8.3+)
fi

TEMP_CPPFLAGS=$CPPFLAGS

CPPFLAGS="$TEMP_CPPFLAGS -I$PG_INCLUDEDIR"
AC_CHECK_HEADER(libpq-fe.h, HAVE_LIBPQFE=1)
if test -n "HAVE_LIBPQFE" ; then
    AC_DEFINE(PG_LIBPQ_VERIFIED,1,[PostgreSQL header check])
else
    AC_MSG_ERROR(Headers for libpq are not found in the includedir.
    Please specify the includedir with --with-pgincludedir=<dir>
    )
fi

CPPFLAGS="$TEMP_CPPFLAGS -I$PG_INCLUDEDIR -I$PG_INCLUDESERVERDIR"

dnl ---------------------------------------------------
dnl Add the port specific include directory if required
dnl ---------------------------------------------------

case "${host_os}" in
        *mingw32*)
        
        CPPFLAGS="$CPPFLAGS -I$PG_INCLUDESERVERDIR/port/win32"
        ;;
esac

AC_CHECK_HEADER(postgres.h, HAVE_LIBPQSERVER=1)
AC_CHECK_HEADER(utils/typcache.h, AC_DEFINE(HAVE_TYPCACHE,1,[PostgreSQL typcache]),[],[#include "postgres.h"])
if test -n "$HAVE_LIBPQSERVER" -a "$HAVE_TYPCACHE" != "yes"; then
    AC_DEFINE(PG_LIBPQSERVER_VERIFIED,1,[PostgreSQL header check])
else
    AC_MSG_ERROR(Headers for libpqserver are not found in the includeserverdir.
    This is the path to postgres.h. Please specify the includeserverdir with 
    --with-pgincludeserverdir=<dir>
    )
fi

AC_MSG_CHECKING(for plpgsql.so)
if test -n "${with_pgpkglibdir}"; then
    AC_MSG_RESULT(skipped due to override)
    AC_DEFINE(PG_PKGLIBDIR_VERIFIED,1,[PostgreSQL pkglibdir])
else
    LDFLAGS="$TEMP_LDFLAGS -L$PG_PKGLIBDIR"

    if test -s $PG_PKGLIBDIR"/plpgsql.so"; then
	AC_MSG_RESULT(yes)
	AC_DEFINE(PG_PKGLIBDIR_VERIFIED,1,[PostgreSQL pkglibdir])
    elif test -s $PG_PKGLIBDIR"/plpgsql.sl"; then
	AC_MSG_RESULT(yes)
	AC_DEFINE(PG_PKGLIBDIR_VERIFIED,1,[PostgreSQL pkglibdir])
    elif test -s $PG_PKGLIBDIR"/plpgsql.dll"; then
	AC_MSG_RESULT(yes)
	AC_DEFINE(PG_PKGLIBDIR_VERIFIED,1,[PostgreSQL pkglibdir])
    else
	AC_MSG_RESULT(no)
	AC_MSG_ERROR($PG_PKGLIBDIR/plpgsql.[so|sl|dll] is not found in the pkglibdir.
	Please specify the pkglibdir with --with-pgpkglibdir=<dir>
	)
    fi
fi

AC_MSG_CHECKING(for postgresql.conf.sample)
if test -n "$PG_SHAREDIR" ; then
    AC_MSG_RESULT(skipped due to override)
    AC_DEFINE(PG_SHAREDIR_VERIFIED,1,[PostgreSQL sharedir])
else
    PGSHARE_POSSIBLE_LOCATIONS="$PG_SHAREDIR /usr/local/pgsql/share /usr/local/share/postgresql /usr/share/postgresql /usr/local/share/pgsql /usr/share/pgsql /opt/local/pgsql/share /opt/pgsql/share ${PG_BINDIR}/../share"
    for i in $PGSHARE_POSSIBLE_LOCATIONS; do
	if test -s "$i/postgresql.conf.sample" ; then
	    PG_SHAREDIR=$i/
	    break;
	fi
    done
    if test -n "$PG_SHAREDIR" ; then
	AC_MSG_RESULT(${PG_SHAREDIR}postgresql.conf.sample)
	AC_DEFINE(PG_SHAREDIR_VERIFIED,1,[PostgreSQL sharedir])
    else
	AC_MSG_RESULT(not found)
	AC_MSG_ERROR(
	postgresql.conf.sample not found! Please specify the sharedir
	with --with-pgsharedir=<dir>
	)
    fi
fi

LDFLAGS="$TEMP_LDFLAGS -L$PG_LIBDIR"

dnl On Windows (and other OSs?) we need to link against libintl if the server
dnl was built with --enable-nls

case "${host_os}" in
    *mingw32*)
        NLSOPT=`echo $PG_CONFIGURE | grep -c enable-nls`
        if test "$NLSOPT" -ne 0; then
            NLSLIB="-lintl"
        fi
        ;;
esac


have_pqputcopydata=no
AC_CHECK_LIB(pq, [PQputCopyData], [have_pqputcopydata=yes])
if test $have_pqputcopydata = yes ; then
        AC_DEFINE(HAVE_PQPUTCOPYDATA,1,[Postgresql PQputCopyData()])
fi

have_pqsetnoticereceiver=no
AC_CHECK_LIB(pq, [PQsetNoticeReceiver], [have_pqsetnoticereceiver=yes])
if test $have_pqsetnoticereceiver = yes; then
	 AC_DEFINE(HAVE_PQSETNOTICERECEIVER,1,[Postgresql PQsetNoticeReceiver()])
fi

have_pqfreemem=no
AC_CHECK_LIB(pq, [PQfreemem], [have_pqfreemem=yes])
if test $have_pqfreemem = yes; then
	AC_DEFINE(HAVE_PQFREEMEM,1,[Postgresql PQfreemem()])
fi


AC_MSG_CHECKING(for typenameTypeId)
if test -z "$ac_cv_typenameTypeId_args"; then
  AC_TRY_COMPILE(
    [#include "postgres.h"
     #include "parser/parse_type.h"],
    [typenameTypeId(NULL, NULL); ], 
    ac_cv_typenameTypeId_args=2)
fi
if test -z "$ac_cv_typenameTypeId_args" ; then
  AC_TRY_COMPILE(
    [#include "postgres.h"
     #include "parser/parse_type.h"],
    [typenameTypeId(NULL); ],
    ac_cv_typenameTypeId_args=1)
fi
if test -z "$ac_cv_typenameTypeId_args"; then
  AC_MSG_RESULT(no)
else
  if test "$ac_cv_typenameTypeId_args" = 2; then
    AC_DEFINE(HAVE_TYPENAMETYPEID_2)
  elif test "$ac_cv_typenameTypeId_args" = 1; then
    AC_DEFINE(HAVE_TYPENAMETYPEID_1)
  fi
  AC_MSG_RESULT([yes, and it takes $ac_cv_typenameTypeId_args arguments])
fi

AC_MSG_CHECKING(for standard_conforming_strings)
if test -z "$ac_cv_standard_conforming_strings"; then
  AC_EGREP_HEADER(standard_conforming_strings, 
    parser/gramparse.h, 
    [AC_MSG_RESULT(yes)
     AC_DEFINE(HAVE_STANDARDCONFORMINGSTRINGS)],
    AC_MSG_RESULT(no)
    )
fi

AC_CHECK_DECLS([GetTopTransactionId],[],[],[
#include "postgres.h"
#include "access/xact.h"
])

AC_SUBST(NLSLIB)

AC_LANG_RESTORE
])dnl ACX_LIBPQ
