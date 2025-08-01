/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | https://www.php.net/license/3_01.txt                                 |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Author: Kristian Koehntopp <kris@koehntopp.de>                       |
   +----------------------------------------------------------------------+
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include <unistd.h>
#include "ext/standard/info.h"
#include "php_posix.h"
#include "main/php_network.h"

#ifdef HAVE_POSIX

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/times.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#ifdef MAJOR_IN_MKDEV
# include <sys/mkdev.h>
#elif defined(MAJOR_IN_SYSMACROS)
# include <sys/sysmacros.h>
#endif

#if (defined(__sun) && !defined(_LP64)) || defined(_AIX)
#define POSIX_PID_MIN LONG_MIN
#define POSIX_PID_MAX LONG_MAX
#else
#define POSIX_PID_MIN INT_MIN
#define POSIX_PID_MAX INT_MAX
#endif

#include "posix_arginfo.h"

ZEND_DECLARE_MODULE_GLOBALS(posix)
static PHP_MINFO_FUNCTION(posix);

/* {{{ PHP_MINFO_FUNCTION */
static PHP_MINFO_FUNCTION(posix)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "POSIX support", "enabled");
	php_info_print_table_end();
}
/* }}} */

static PHP_GINIT_FUNCTION(posix) /* {{{ */
{
#if defined(COMPILE_DL_POSIX) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	posix_globals->last_error = 0;
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION(posix) */
static PHP_MINIT_FUNCTION(posix)
{
	register_posix_symbols(module_number);

	return SUCCESS;
}
/* }}} */

/* {{{ posix_module_entry */
zend_module_entry posix_module_entry = {
	STANDARD_MODULE_HEADER,
	"posix",
	ext_functions,
	PHP_MINIT(posix),
	NULL,
	NULL,
	NULL,
	PHP_MINFO(posix),
	PHP_POSIX_VERSION,
	PHP_MODULE_GLOBALS(posix),
	PHP_GINIT(posix),
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};
/* }}} */

#ifdef COMPILE_DL_POSIX
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(posix)
#endif

#define PHP_POSIX_RETURN_LONG_FUNC(func_name)	\
	ZEND_PARSE_PARAMETERS_NONE();	\
	RETURN_LONG(func_name());

#define PHP_POSIX_SINGLE_ARG_FUNC(func_name)	\
	zend_long val;	\
	ZEND_PARSE_PARAMETERS_START(1, 1) \
		Z_PARAM_LONG(val) \
	ZEND_PARSE_PARAMETERS_END(); \
	if (func_name(val) < 0) {	\
		POSIX_G(last_error) = errno;	\
		RETURN_FALSE;	\
	}	\
	RETURN_TRUE;

#define PHP_POSIX_CHECK_PID(pid, lower, upper)										\
	if (pid < lower || pid > upper) {										\
		zend_argument_value_error(1, "must be between " ZEND_LONG_FMT " and " ZEND_LONG_FMT, lower, upper);	\
		RETURN_THROWS();											\
	}

/* {{{ Send a signal to a process (POSIX.1, 3.3.2) */

PHP_FUNCTION(posix_kill)
{
	zend_long pid, sig;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_LONG(pid)
		Z_PARAM_LONG(sig)
	ZEND_PARSE_PARAMETERS_END();

	PHP_POSIX_CHECK_PID(pid, POSIX_PID_MIN, POSIX_PID_MAX)

	if (kill(pid, sig) < 0) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Get the current process id (POSIX.1, 4.1.1) */
PHP_FUNCTION(posix_getpid)
{
	PHP_POSIX_RETURN_LONG_FUNC(getpid);
}
/* }}} */

/* {{{ Get the parent process id (POSIX.1, 4.1.1) */
PHP_FUNCTION(posix_getppid)
{
	PHP_POSIX_RETURN_LONG_FUNC(getppid);
}
/* }}} */

/* {{{ Get the current user id (POSIX.1, 4.2.1) */
PHP_FUNCTION(posix_getuid)
{
	PHP_POSIX_RETURN_LONG_FUNC(getuid);
}
/* }}} */

/* {{{ Get the current group id (POSIX.1, 4.2.1) */
PHP_FUNCTION(posix_getgid)
{
	PHP_POSIX_RETURN_LONG_FUNC(getgid);
}
/* }}} */

/* {{{ Get the current effective user id (POSIX.1, 4.2.1) */
PHP_FUNCTION(posix_geteuid)
{
	PHP_POSIX_RETURN_LONG_FUNC(geteuid);
}
/* }}} */

/* {{{ Get the current effective group id (POSIX.1, 4.2.1) */
PHP_FUNCTION(posix_getegid)
{
	PHP_POSIX_RETURN_LONG_FUNC(getegid);
}
/* }}} */

/* {{{ Set user id (POSIX.1, 4.2.2) */
PHP_FUNCTION(posix_setuid)
{
	PHP_POSIX_SINGLE_ARG_FUNC(setuid);
}
/* }}} */

/* {{{ Set group id (POSIX.1, 4.2.2) */
PHP_FUNCTION(posix_setgid)
{
	PHP_POSIX_SINGLE_ARG_FUNC(setgid);
}
/* }}} */

/* {{{ Set effective user id */
#ifdef HAVE_SETEUID
PHP_FUNCTION(posix_seteuid)
{
	PHP_POSIX_SINGLE_ARG_FUNC(seteuid);
}
#endif
/* }}} */

/* {{{ Set effective group id */
#ifdef HAVE_SETEGID
PHP_FUNCTION(posix_setegid)
{
	PHP_POSIX_SINGLE_ARG_FUNC(setegid);
}
#endif
/* }}} */

/* {{{ Get supplementary group id's (POSIX.1, 4.2.3) */
#ifdef HAVE_GETGROUPS
PHP_FUNCTION(posix_getgroups)
{
	gid_t *gidlist;
	int    result;
	int    i;

	ZEND_PARSE_PARAMETERS_NONE();

	/* MacOS may return more than NGROUPS_MAX groups.
	 * Fetch the actual number of groups and create an appropriate allocation. */
	if ((result = getgroups(0, NULL)) < 0) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	gidlist = emalloc(sizeof(gid_t) * result);
	if ((result = getgroups(result, gidlist)) < 0) {
		POSIX_G(last_error) = errno;
		efree(gidlist);
		RETURN_FALSE;
	}

	array_init_size(return_value, result);
	zend_hash_real_init_packed(Z_ARRVAL_P(return_value));

	for (i=0; i<result; i++) {
		add_index_long(return_value, i, gidlist[i]);
	}
	efree(gidlist);
}
#endif
/* }}} */

/* {{{ Get user name (POSIX.1, 4.2.4) */
#ifdef HAVE_GETLOGIN
PHP_FUNCTION(posix_getlogin)
{
	char *p;

	ZEND_PARSE_PARAMETERS_NONE();

	if (NULL == (p = getlogin())) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	RETURN_STRING(p);
}
#endif
/* }}} */

/* {{{ Get current process group id (POSIX.1, 4.3.1) */
PHP_FUNCTION(posix_getpgrp)
{
	PHP_POSIX_RETURN_LONG_FUNC(getpgrp);
}
/* }}} */

/* {{{ Create session and set process group id (POSIX.1, 4.3.2) */
#ifdef HAVE_SETSID
PHP_FUNCTION(posix_setsid)
{
	PHP_POSIX_RETURN_LONG_FUNC(setsid);
}
#endif
/* }}} */

/* {{{ Set process group id for job control (POSIX.1, 4.3.3) */
PHP_FUNCTION(posix_setpgid)
{
	zend_long pid, pgid;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_LONG(pid)
		Z_PARAM_LONG(pgid)
	ZEND_PARSE_PARAMETERS_END();

	PHP_POSIX_CHECK_PID(pid, 0, POSIX_PID_MAX)

	if (setpgid(pid, pgid) < 0) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Get the process group id of the specified process (This is not a POSIX function, but a SVR4ism, so we compile conditionally) */
#ifdef HAVE_GETPGID
PHP_FUNCTION(posix_getpgid)
{
	zend_long val;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(val)
	ZEND_PARSE_PARAMETERS_END();

	if ((val = getpgid(val)) < 0) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}
	RETURN_LONG(val);
}
#endif
/* }}} */

/* {{{ Get process group id of session leader (This is not a POSIX function, but a SVR4ism, so be compile conditionally) */
#ifdef HAVE_GETSID
PHP_FUNCTION(posix_getsid)
{
	zend_long val;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(val)
	ZEND_PARSE_PARAMETERS_END();

	if ((val = getsid(val)) < 0) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}
	RETURN_LONG(val);
}
#endif
/* }}} */

/* {{{ Get system name (POSIX.1, 4.4.1) */
PHP_FUNCTION(posix_uname)
{
	struct utsname u;

	ZEND_PARSE_PARAMETERS_NONE();

	if (uname(&u) < 0) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	array_init(return_value);

	add_assoc_string(return_value, "sysname",  u.sysname);
	add_assoc_string(return_value, "nodename", u.nodename);
	add_assoc_string(return_value, "release",  u.release);
	add_assoc_string(return_value, "version",  u.version);
	add_assoc_string(return_value, "machine",  u.machine);

#if defined(_GNU_SOURCE) && defined(HAVE_STRUCT_UTSNAME_DOMAINNAME)
	add_assoc_string(return_value, "domainname", u.domainname);
#endif
}
/* }}} */

/* POSIX.1, 4.5.1 time() - Get System Time
							already covered by PHP
 */

/* {{{ Get process times (POSIX.1, 4.5.2) */
PHP_FUNCTION(posix_times)
{
	struct tms t;
	clock_t    ticks;

	ZEND_PARSE_PARAMETERS_NONE();

	if ((ticks = times(&t)) == -1) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	array_init_size(return_value, 5);

	add_assoc_long(return_value, "ticks",	ticks);			/* clock ticks */
	add_assoc_long(return_value, "utime",	t.tms_utime);	/* user time */
	add_assoc_long(return_value, "stime",	t.tms_stime);	/* system time */
	add_assoc_long(return_value, "cutime",	t.tms_cutime);	/* user time of children */
	add_assoc_long(return_value, "cstime",	t.tms_cstime);	/* system time of children */
}
/* }}} */

/* POSIX.1, 4.6.1 getenv() - Environment Access
							already covered by PHP
*/

/* {{{ Generate terminal path name (POSIX.1, 4.7.1) */
#ifdef HAVE_CTERMID
PHP_FUNCTION(posix_ctermid)
{
	char  buffer[L_ctermid];

	ZEND_PARSE_PARAMETERS_NONE();

	if (NULL == ctermid(buffer)) {
		/* Found no documentation how the defined behaviour is when this
		 * function fails
		 */
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	RETURN_STRING(buffer);
}
#endif
/* }}} */

/* Checks if the provides resource is a stream and if it provides a file descriptor */
static zend_result php_posix_stream_get_fd(zval *zfp, zend_long *ret) /* {{{ */
{
	php_stream *stream;

	php_stream_from_zval_no_verify(stream, zfp);

	if (stream == NULL) {
		return FAILURE;
	}

	/* get the fd. php_socket_t is used for FDs, and is shorter than zend_long.
	 * NB: Most other code will NOT use the PHP_STREAM_CAST_INTERNAL flag when casting.
	 * It is only used here so that the buffered data warning is not displayed.
	 */
	php_socket_t fd = -1;
	if (php_stream_can_cast(stream, PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL) == SUCCESS) {
		php_stream_cast(stream, PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL, (void**)&fd, 0);
	} else if (php_stream_can_cast(stream, PHP_STREAM_AS_FD | PHP_STREAM_CAST_INTERNAL) == SUCCESS) {
		php_stream_cast(stream, PHP_STREAM_AS_FD | PHP_STREAM_CAST_INTERNAL, (void**)&fd, 0);
	} else {
		php_error_docref(NULL, E_WARNING, "Could not use stream of type '%s'",
				stream->ops->label);
		return FAILURE;
	}
	*ret = fd;
	return SUCCESS;
}
/* }}} */

/* {{{ Determine terminal device name (POSIX.1, 4.7.2) */
PHP_FUNCTION(posix_ttyname)
{
	zval *z_fd;
	char *p;
	zend_long fd = 0;
#if defined(ZTS) && defined(HAVE_TTYNAME_R) && defined(_SC_TTY_NAME_MAX)
	zend_long buflen;
	int err;
#endif

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(z_fd)
	ZEND_PARSE_PARAMETERS_END();

	if (Z_TYPE_P(z_fd) == IS_RESOURCE) {
		if (php_posix_stream_get_fd(z_fd, &fd) == FAILURE) {
			RETURN_FALSE;
		}
	} else {
		if (!zend_parse_arg_long(z_fd, &fd, /* is_null */ NULL, /* check_null */ false, /* arg_num */ 1)) {
			php_error_docref(NULL, E_WARNING, "Argument #1 ($file_descriptor) must be of type int|resource, %s given",
				zend_zval_value_name(z_fd));
			fd = zval_get_long(z_fd);
		}
		/* fd must fit in an int and be positive */
		if (fd < 0 || fd > INT_MAX) {
			php_error_docref(NULL, E_WARNING, "Argument #1 ($file_descriptor) must be between 0 and %d", INT_MAX);
			POSIX_G(last_error) = EBADF;
			RETURN_FALSE;
		}
	}
#if defined(ZTS) && defined(HAVE_TTYNAME_R) && defined(_SC_TTY_NAME_MAX)
	buflen = sysconf(_SC_TTY_NAME_MAX);
	if (buflen < 1) {
		buflen = 32;
	}
#if ZEND_DEBUG
	/* Test retry logic */
	buflen = 1;
#endif
	p = emalloc(buflen);

try_again:
	err = ttyname_r(fd, p, buflen);
	if (err) {
		if (err == ERANGE) {
			buflen *= 2;
			p = erealloc(p, buflen);
			goto try_again;
		}
		POSIX_G(last_error) = err;
		efree(p);
		RETURN_FALSE;
	}
	RETVAL_STRING(p);
	efree(p);
#else
	if (NULL == (p = ttyname(fd))) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}
	RETURN_STRING(p);
#endif
}
/* }}} */

/* {{{ Determine if filedesc is a tty (POSIX.1, 4.7.1) */
PHP_FUNCTION(posix_isatty)
{
	zval *z_fd;
	zend_long fd = 0;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(z_fd)
	ZEND_PARSE_PARAMETERS_END();

	if (Z_TYPE_P(z_fd) == IS_RESOURCE) {
		if (php_posix_stream_get_fd(z_fd, &fd) == FAILURE) {
			RETURN_FALSE;
		}
	} else {
		if (!zend_parse_arg_long(z_fd, &fd, /* is_null */ NULL, /* check_null */ false, /* arg_num */ 1)) {
			php_error_docref(NULL, E_WARNING, "Argument #1 ($file_descriptor) must be of type int|resource, %s given",
				zend_zval_value_name(z_fd));
			RETURN_FALSE;
		}
	}

	/* A valid file descriptor must fit in an int and be positive */
	if (fd < 0 || fd > INT_MAX) {
		php_error_docref(NULL, E_WARNING, "Argument #1 ($file_descriptor) must be between 0 and %d", INT_MAX);
		POSIX_G(last_error) = EBADF;
		RETURN_FALSE;
	}
	if (isatty(fd)) {
		RETURN_TRUE;
	} else {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}
}
/* }}} */

/*
	POSIX.1, 4.8.1 sysconf()
	POSIX.1, 5.7.1 pathconf(), fpathconf()

	POSIX.1, 5.1.2 opendir(), readdir(), rewinddir(), closedir()
	POSIX.1, 5.2.1 chdir()
				already supported by PHP
 */

/* {{{ Get working directory pathname (POSIX.1, 5.2.2) */
PHP_FUNCTION(posix_getcwd)
{
	char  buffer[MAXPATHLEN];
	char *p;

	ZEND_PARSE_PARAMETERS_NONE();

	p = VCWD_GETCWD(buffer, MAXPATHLEN);
	if (!p) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	RETURN_STRING(buffer);
}
/* }}} */

/*
	POSIX.1, 5.3.x open(), creat(), umask()
	POSIX.1, 5.4.1 link()
		already supported by PHP.
 */

/* {{{ Make a FIFO special file (POSIX.1, 5.4.2) */
#ifdef HAVE_MKFIFO
PHP_FUNCTION(posix_mkfifo)
{
	zend_string *path;
	zend_long mode;
	int     result;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_PATH_STR(path)
		Z_PARAM_LONG(mode)
	ZEND_PARSE_PARAMETERS_END();

	if (php_check_open_basedir_ex(ZSTR_VAL(path), 0)) {
		RETURN_FALSE;
	}

	result = mkfifo(ZSTR_VAL(path), mode);
	if (result < 0) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
#endif
/* }}} */

/* {{{ Make a special or ordinary file (POSIX.1) */
#ifdef HAVE_MKNOD
PHP_FUNCTION(posix_mknod)
{
	zend_string *path;
	zend_long mode;
	zend_long major = 0, minor = 0;
	int result;
	dev_t php_dev = 0;

	ZEND_PARSE_PARAMETERS_START(2, 4)
		Z_PARAM_PATH_STR(path)
		Z_PARAM_LONG(mode)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(major)
		Z_PARAM_LONG(minor)
	ZEND_PARSE_PARAMETERS_END();

	if (php_check_open_basedir_ex(ZSTR_VAL(path), 0)) {
		RETURN_FALSE;
	}

	if ((mode & S_IFCHR) || (mode & S_IFBLK)) {
		if (major == 0) {
			zend_argument_value_error(3, "cannot be 0 for the POSIX_S_IFCHR and POSIX_S_IFBLK modes");
			RETURN_THROWS();
		} else {
#ifdef HAVE_MAKEDEV
			php_dev = makedev(major, minor);
#else
			php_error_docref(NULL, E_WARNING, "Cannot create a block or character device, creating a normal file instead");
#endif
		}
	}

	result = mknod(ZSTR_VAL(path), mode, php_dev);
	if (result < 0) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
#endif
/* }}} */

/* Takes a pointer to posix group and a pointer to an already initialized ZVAL
 * array container and fills the array with the posix group member data. */
static void php_posix_group_to_array(struct group *g, zval *array_group) /* {{{ */
{
	zval array_members;
	int count;

	ZEND_ASSERT(Z_TYPE_P(array_group) == IS_ARRAY);

	array_init(&array_members);
	zend_hash_real_init_packed(Z_ARRVAL(array_members));

	add_assoc_string(array_group, "name", g->gr_name);
	if (g->gr_passwd) {
		add_assoc_string(array_group, "passwd", g->gr_passwd);
	} else {
		add_assoc_null(array_group, "passwd");
	}
	for (count = 0;; count++) {
		/* gr_mem entries may be misaligned on macos. */
		char *gr_mem;
		memcpy(&gr_mem, &g->gr_mem[count], sizeof(char *));
		if (!gr_mem) {
			break;
		}

		add_next_index_string(&array_members, gr_mem);
	}
	zend_hash_str_update(Z_ARRVAL_P(array_group), "members", sizeof("members")-1, &array_members);
	add_assoc_long(array_group, "gid", g->gr_gid);
}
/* }}} */

/*
	POSIX.1, 5.5.1 unlink()
	POSIX.1, 5.5.2 rmdir()
	POSIX.1, 5.5.3 rename()
	POSIX.1, 5.6.x stat(), chmod(), utime() already supported by PHP.
*/

/* {{{ Determine accessibility of a file (POSIX.1 5.6.3) */
PHP_FUNCTION(posix_access)
{
	zend_long mode = 0;
	size_t filename_len, ret;
	char *filename, *path;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_PATH(filename, filename_len)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(mode)
	ZEND_PARSE_PARAMETERS_END();

	path = expand_filepath(filename, NULL);
	if (!path) {
		POSIX_G(last_error) = EIO;
		RETURN_FALSE;
	}

	if (php_check_open_basedir_ex(path, 0)) {
		efree(path);
		POSIX_G(last_error) = EPERM;
		RETURN_FALSE;
	}

	ret = access(path, mode);
	efree(path);

	if (ret) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	RETURN_TRUE;
}

#ifdef HAVE_EACCESS
PHP_FUNCTION(posix_eaccess)
{
	zend_long mode = 0;
	size_t filename_len, ret;
	char *filename, *path;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_PATH(filename, filename_len)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(mode)
	ZEND_PARSE_PARAMETERS_END();

	path = expand_filepath(filename, NULL);
	if (!path) {
		zend_argument_must_not_be_empty_error(1);
		RETURN_THROWS();
	}

	if (php_check_open_basedir_ex(path, 0)) {
		efree(path);
		POSIX_G(last_error) = EPERM;
		RETURN_FALSE;
	}

	ret = eaccess(path, mode);
	efree(path);

	if (ret) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
#endif

/* }}} */

/*
	POSIX.1, 6.x most I/O functions already supported by PHP.
	POSIX.1, 7.x tty functions, TODO
	POSIX.1, 8.x interactions with other C language functions
	POSIX.1, 9.x system database access
*/

/* {{{ Group database access (POSIX.1, 9.2.1) */
PHP_FUNCTION(posix_getgrnam)
{
	char *name;
	struct group *g;
	size_t name_len;
#if defined(ZTS) && defined(HAVE_GETGRNAM_R) && defined(_SC_GETGR_R_SIZE_MAX)
	struct group gbuf;
	long buflen;
	char *buf;
	int err;
#endif

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STRING(name, name_len)
	ZEND_PARSE_PARAMETERS_END();

#if defined(ZTS) && defined(HAVE_GETGRNAM_R) && defined(_SC_GETGR_R_SIZE_MAX)
	buflen = sysconf(_SC_GETGR_R_SIZE_MAX);
	if (buflen < 1) {
		buflen = 1024;
	}
#if ZEND_DEBUG
	/* Test retry logic */
	buflen = 1;
#endif
	buf = emalloc(buflen);
try_again:
	g = &gbuf;

	err = getgrnam_r(name, g, buf, buflen, &g);
	if (err || g == NULL) {
		if (err == ERANGE) {
			buflen *= 2;
			buf = erealloc(buf, buflen);
			goto try_again;
		}
		POSIX_G(last_error) = err;
		efree(buf);
		RETURN_FALSE;
	}
#else
	if (NULL == (g = getgrnam(name))) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}
#endif
	array_init(return_value);

	php_posix_group_to_array(g, return_value);
#if defined(ZTS) && defined(HAVE_GETGRNAM_R) && defined(_SC_GETGR_R_SIZE_MAX)
	efree(buf);
#endif
}
/* }}} */

/* {{{ Group database access (POSIX.1, 9.2.1) */
PHP_FUNCTION(posix_getgrgid)
{
	zend_long gid;
#if defined(ZTS) && defined(HAVE_GETGRGID_R) && defined(_SC_GETGR_R_SIZE_MAX)
	int err;
	struct group _g;
	struct group *retgrptr = NULL;
	long grbuflen;
	char *grbuf;
#endif
	struct group *g;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(gid)
	ZEND_PARSE_PARAMETERS_END();

#if defined(ZTS) && defined(HAVE_GETGRGID_R) && defined(_SC_GETGR_R_SIZE_MAX)

	grbuflen = sysconf(_SC_GETGR_R_SIZE_MAX);
	if (grbuflen < 1) {
		grbuflen = 1024;
	}
#if ZEND_DEBUG
	/* Test retry logic */
	grbuflen = 1;
#endif

	grbuf = emalloc(grbuflen);

try_again:
	err = getgrgid_r(gid, &_g, grbuf, grbuflen, &retgrptr);
	if (err || retgrptr == NULL) {
		if (err == ERANGE) {
			grbuflen *= 2;
			grbuf = erealloc(grbuf, grbuflen);
			goto try_again;
		}
		POSIX_G(last_error) = err;
		efree(grbuf);
		RETURN_FALSE;
	}
	g = &_g;
#else
	if (NULL == (g = getgrgid(gid))) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}
#endif
	array_init(return_value);

	php_posix_group_to_array(g, return_value);
#if defined(ZTS) && defined(HAVE_GETGRGID_R) && defined(_SC_GETGR_R_SIZE_MAX)
	efree(grbuf);
#endif
}
/* }}} */

static void php_posix_passwd_to_array(struct passwd *pw, zval *return_value) /* {{{ */
{
	ZEND_ASSERT(Z_TYPE_P(return_value) == IS_ARRAY);

	add_assoc_string(return_value, "name",      pw->pw_name);
	add_assoc_string(return_value, "passwd",    pw->pw_passwd);
	add_assoc_long  (return_value, "uid",       pw->pw_uid);
	add_assoc_long  (return_value, "gid",		pw->pw_gid);
	add_assoc_string(return_value, "gecos",     pw->pw_gecos);
	add_assoc_string(return_value, "dir",       pw->pw_dir);
	add_assoc_string(return_value, "shell",     pw->pw_shell);
}
/* }}} */

/* {{{ User database access (POSIX.1, 9.2.2) */
PHP_FUNCTION(posix_getpwnam)
{
	struct passwd *pw;
	char *name;
	size_t name_len;
#if defined(ZTS) && defined(_SC_GETPW_R_SIZE_MAX) && defined(HAVE_GETPWNAM_R)
	struct passwd pwbuf;
	long buflen;
	char *buf;
	int err;
#endif

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STRING(name, name_len)
	ZEND_PARSE_PARAMETERS_END();

#if defined(ZTS) && defined(_SC_GETPW_R_SIZE_MAX) && defined(HAVE_GETPWNAM_R)
	buflen = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (buflen < 1) {
		buflen = 1024;
	}
#if ZEND_DEBUG
	/* Test retry logic */
	buflen = 1;
#endif
	buf = emalloc(buflen);

try_again:
	pw = &pwbuf;
	err = getpwnam_r(name, pw, buf, buflen, &pw);
	if (err || pw == NULL) {
		if (err == ERANGE) {
			buflen *= 2;
			buf = erealloc(buf, buflen);
			goto try_again;
		}
		efree(buf);
		POSIX_G(last_error) = err;
		RETURN_FALSE;
	}
#else
	if (NULL == (pw = getpwnam(name))) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}
#endif
	array_init(return_value);

	php_posix_passwd_to_array(pw, return_value);
#if defined(ZTS) && defined(_SC_GETPW_R_SIZE_MAX) && defined(HAVE_GETPWNAM_R)
	efree(buf);
#endif
}
/* }}} */

/* {{{ User database access (POSIX.1, 9.2.2) */
PHP_FUNCTION(posix_getpwuid)
{
	zend_long uid;
#if defined(ZTS) && defined(_SC_GETPW_R_SIZE_MAX) && defined(HAVE_GETPWUID_R)
	struct passwd _pw;
	struct passwd *retpwptr = NULL;
	long pwbuflen;
	char *pwbuf;
	int err;
#endif
	struct passwd *pw;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(uid)
	ZEND_PARSE_PARAMETERS_END();

#if defined(ZTS) && defined(_SC_GETPW_R_SIZE_MAX) && defined(HAVE_GETPWUID_R)
	pwbuflen = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (pwbuflen < 1) {
		pwbuflen = 1024;
	}
#if ZEND_DEBUG
	/* Test retry logic */
	pwbuflen = 1;
#endif
	pwbuf = emalloc(pwbuflen);

try_again:
	err = getpwuid_r(uid, &_pw, pwbuf, pwbuflen, &retpwptr);
	if (err || retpwptr == NULL) {
		if (err == ERANGE) {
			pwbuflen *= 2;
			pwbuf = erealloc(pwbuf, pwbuflen);
			goto try_again;
		}
		POSIX_G(last_error) = err;
		efree(pwbuf);
		RETURN_FALSE;
	}
	pw = &_pw;
#else
	if (NULL == (pw = getpwuid(uid))) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}
#endif
	array_init(return_value);

	php_posix_passwd_to_array(pw, return_value);
#if defined(ZTS) && defined(_SC_GETPW_R_SIZE_MAX) && defined(HAVE_GETPWUID_R)
	efree(pwbuf);
#endif
}
/* }}} */


#ifdef HAVE_GETRLIMIT

#define UNLIMITED_STRING "unlimited"

/* {{{ posix_addlimit */
static zend_result posix_addlimit(int limit, const char *name, zval *return_value) {
	int result;
	struct rlimit rl;
	char hard[80];
	char soft[80];

	snprintf(hard, 80, "hard %s", name);
	snprintf(soft, 80, "soft %s", name);

	result = getrlimit(limit, &rl);
	if (result < 0) {
		POSIX_G(last_error) = errno;
		return FAILURE;
	}

	if (rl.rlim_cur == RLIM_INFINITY) {
		add_assoc_stringl(return_value, soft, UNLIMITED_STRING, sizeof(UNLIMITED_STRING)-1);
	} else {
		add_assoc_long(return_value, soft, rl.rlim_cur);
	}

	if (rl.rlim_max == RLIM_INFINITY) {
		add_assoc_stringl(return_value, hard, UNLIMITED_STRING, sizeof(UNLIMITED_STRING)-1);
	} else {
		add_assoc_long(return_value, hard, rl.rlim_max);
	}

	return SUCCESS;
}
/* }}} */

/* {{{ limits[] */
static const struct limitlist {
	int limit;
	const char *name;
} limits[] = {
#ifdef RLIMIT_CORE
	{ RLIMIT_CORE,	"core" },
#endif

#ifdef RLIMIT_DATA
	{ RLIMIT_DATA,	"data" },
#endif

#ifdef RLIMIT_STACK
	{ RLIMIT_STACK,	"stack" },
#endif

#ifdef RLIMIT_VMEM
	{ RLIMIT_VMEM, "virtualmem" },
#endif

#ifdef RLIMIT_AS
	{ RLIMIT_AS, "totalmem" },
#endif

#ifdef RLIMIT_RSS
	{ RLIMIT_RSS, "rss" },
#endif

#ifdef RLIMIT_NPROC
	{ RLIMIT_NPROC, "maxproc" },
#endif

#ifdef RLIMIT_MEMLOCK
	{ RLIMIT_MEMLOCK, "memlock" },
#endif

#ifdef RLIMIT_CPU
	{ RLIMIT_CPU,	"cpu" },
#endif

#ifdef RLIMIT_FSIZE
	{ RLIMIT_FSIZE, "filesize" },
#endif

#ifdef RLIMIT_NOFILE
	{ RLIMIT_NOFILE, "openfiles" },
#endif

#ifdef RLIMIT_OFILE
	{ RLIMIT_OFILE, "openfiles" },
#endif

#ifdef RLIMIT_KQUEUES
	{ RLIMIT_KQUEUES, "kqueues" },
#endif

#ifdef RLIMIT_NPTS
	{ RLIMIT_NPTS, "npts" },
#endif

	{ 0, NULL }
};
/* }}} */


/* {{{ Get system resource consumption limits (This is not a POSIX function, but a BSDism and a SVR4ism. We compile conditionally) */
PHP_FUNCTION(posix_getrlimit)
{
	const struct limitlist *l = NULL;
	zend_long res;
	bool res_is_null = true;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG_OR_NULL(res, res_is_null)
	ZEND_PARSE_PARAMETERS_END();

	if (res_is_null) {
		array_init(return_value);

		for (l=limits; l->name; l++) {
			if (posix_addlimit(l->limit, l->name, return_value) == FAILURE) {
				zend_array_destroy(Z_ARR_P(return_value));
				RETURN_FALSE;
			}
		}
	} else {
		struct rlimit rl;
		int result = getrlimit(res, &rl);
		if (result < 0) {
			POSIX_G(last_error) = errno;
			RETURN_FALSE;
		}

		array_init_size(return_value, 2);
		zend_hash_real_init_packed(Z_ARRVAL_P(return_value));
		if (rl.rlim_cur == RLIM_INFINITY) {
			add_next_index_stringl(return_value, UNLIMITED_STRING, sizeof(UNLIMITED_STRING)-1);
		} else {
			add_next_index_long(return_value, rl.rlim_cur);
		}

		if (rl.rlim_max == RLIM_INFINITY) {
			add_next_index_stringl(return_value, UNLIMITED_STRING, sizeof(UNLIMITED_STRING)-1);
		} else {
			add_next_index_long(return_value, rl.rlim_max);
		}
	}
}
/* }}} */

#endif /* HAVE_GETRLIMIT */

#ifdef HAVE_SETRLIMIT
/* {{{ Set system resource consumption limits (POSIX.1-2001) */
PHP_FUNCTION(posix_setrlimit)
{
	struct rlimit rl;
	zend_long res, cur, max;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_LONG(res)
		Z_PARAM_LONG(cur)
		Z_PARAM_LONG(max)
	ZEND_PARSE_PARAMETERS_END();

	rl.rlim_cur = cur;
	rl.rlim_max = max;

	if (setrlimit(res, &rl) == -1) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

#endif /* HAVE_SETRLIMIT */


/* {{{ Retrieve the error number set by the last posix function which failed. */
PHP_FUNCTION(posix_get_last_error)
{
	ZEND_PARSE_PARAMETERS_NONE();

	RETURN_LONG(POSIX_G(last_error));
}
/* }}} */

/* {{{ Retrieve the system error message associated with the given errno. */
PHP_FUNCTION(posix_strerror)
{
	zend_long error;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(error)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_STRING(strerror(error));
}
/* }}} */

#endif

#ifdef HAVE_INITGROUPS
/* {{{ Calculate the group access list for the user specified in name. */
PHP_FUNCTION(posix_initgroups)
{
	zend_long basegid;
	char *name;
	size_t name_len;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_STRING(name, name_len)
		Z_PARAM_LONG(basegid)
	ZEND_PARSE_PARAMETERS_END();

	if (name_len == 0) {
		RETURN_FALSE;
	}

	RETURN_BOOL(!initgroups((const char *)name, basegid));
}
/* }}} */
#endif

PHP_FUNCTION(posix_sysconf)
{
	zend_long conf_id;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(conf_id)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_LONG(sysconf(conf_id));
}

#ifdef HAVE_PATHCONF
PHP_FUNCTION(posix_pathconf)
{
	zend_long name, ret;
	char *path;
	size_t path_len;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_PATH(path, path_len)
		Z_PARAM_LONG(name);
	ZEND_PARSE_PARAMETERS_END();

	if (path_len == 0) {
		zend_argument_must_not_be_empty_error(1);
		RETURN_THROWS();
	} else if (php_check_open_basedir(path)) {
		php_error_docref(NULL, E_WARNING, "Invalid path supplied: %s", path);
		RETURN_FALSE;
	}

	ret = pathconf(path, name);

	if (ret < 0 && errno != 0) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	RETURN_LONG(ret);
}
#endif

#ifdef HAVE_FPATHCONF
PHP_FUNCTION(posix_fpathconf)
{
	zend_long name, ret, fd = 0;
	zval *z_fd;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_ZVAL(z_fd)
		Z_PARAM_LONG(name);
	ZEND_PARSE_PARAMETERS_END();

	if (Z_TYPE_P(z_fd) == IS_RESOURCE) {
		if (php_posix_stream_get_fd(z_fd, &fd) == FAILURE) {
			RETURN_FALSE;
		}
	} else {
		if (!zend_parse_arg_long(z_fd, &fd, /* is_null */ NULL, /* check_null */ false, /* arg_num */ 1)) {
			zend_argument_type_error(1, "must be of type int|resource, %s given",
				zend_zval_value_name(z_fd));
			RETURN_THROWS();
		}
	}
	/* fd must fit in an int and be positive */
	if (fd < 0 || fd > INT_MAX) {
		php_error_docref(NULL, E_WARNING, "Argument #1 ($file_descriptor) must be between 0 and %d", INT_MAX);
		POSIX_G(last_error) = EBADF;
		RETURN_FALSE;
	}

	ret = fpathconf(fd, name);

	if (ret < 0 && errno != 0) {
		POSIX_G(last_error) = errno;
		RETURN_FALSE;
	}

	RETURN_LONG(ret);
}
#endif
