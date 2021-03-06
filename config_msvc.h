
#ifndef	SLONY_I_CONFIG_H
#define SLONY_I_CONFIG_H

#include <server/pg_config.h>

#define SLONY_I_VERSION_STRING	"2.1.4"
#define SLONY_I_VERSION_STRING_DEC 2,1,4
#if PG_VERSION_NUM >= 90600
#define HAVE_GETACTIVESNAPSHOT 1
#define HAVE_TYPCACHE 1
#define SETCONFIGOPTION_8 1
#define GETCONFIGOPTIONBYNAME_3
#if PG_VERSION_NUM >= 90500
#define HAVE_GETACTIVESNAPSHOT 1
#define HAVE_TYPCACHE 1
#define SETCONFIGOPTION_8 1
#define GETCONFIGOPTIONBYNAME_2
#elif PG_VERSION_NUM >= 90200
#define HAVE_GETACTIVESNAPSHOT 1
#define HAVE_TYPCACHE 1
#define SETCONFIGOPTION_7 1
#define GETCONFIGOPTIONBYNAME_2
#elif PG_VERSION_NUM >= 90000
#define HAVE_GETACTIVESNAPSHOT 1
#define HAVE_TYPCACHE 1
#define SETCONFIGOPTION_6 1
#define GETCONFIGOPTIONBYNAME_2
#elif PG_VERSION_NUM >=  80400
#define HAVE_GETACTIVESNAPSHOT 1
#define HAVE_TYPCACHE 1
#define SETCONFIGOPTION_6 1
#define GETCONFIGOPTIONBYNAME_2
#elif PG_VERSION_NUM >=  80300
#define HAVE_TYPCACHE 1
#define SETCONFIGOPTION_6 1
#define GETCONFIGOPTIONBYNAME_2
#else
#error "Postgresql 8.3 or higher is required"
#endif

#endif /* SLONY_I_CONFIG_H */
