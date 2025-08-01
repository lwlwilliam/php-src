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
   | Authors: Sascha Schumann <sascha@schumann.cx>                        |
   |          Andrei Zmievski <andrei@php.net>                            |
   +----------------------------------------------------------------------+
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"

#ifdef PHP_WIN32
# include "win32/winutil.h"
# include "win32/time.h"
#else
# include <sys/time.h>
#endif

#include <sys/stat.h>
#include <fcntl.h>

#include "php_ini.h"
#include "rfc1867.h"
#include "php_variables.h"
#include "php_session.h"
#include "session_arginfo.h"
#include "ext/standard/php_var.h"
#include "ext/date/php_date.h"
#include "ext/standard/url_scanner_ex.h"
#include "ext/standard/info.h"
#include "zend_smart_str.h"
#include "ext/standard/url.h"
#include "ext/standard/basic_functions.h"
#include "ext/standard/head.h"
#include "ext/random/php_random.h"
#include "ext/random/php_random_csprng.h"

#include "mod_files.h"
#include "mod_user.h"

#ifdef HAVE_LIBMM
#include "mod_mm.h"
#endif

PHPAPI ZEND_DECLARE_MODULE_GLOBALS(ps)

static zend_result php_session_rfc1867_callback(unsigned int event, void *event_data, void **extra);
static zend_result (*php_session_rfc1867_orig_callback)(unsigned int event, void *event_data, void **extra);
static void php_session_track_init(void);

/* SessionHandler class */
zend_class_entry *php_session_class_entry;

/* SessionHandlerInterface */
zend_class_entry *php_session_iface_entry;

/* SessionIdInterface */
zend_class_entry *php_session_id_iface_entry;

/* SessionUpdateTimestampInterface */
zend_class_entry *php_session_update_timestamp_iface_entry;

#define PS_MAX_SID_LENGTH 256

/* ***********
   * Helpers *
   *********** */

#define IF_SESSION_VARS() \
	if (Z_ISREF_P(&PS(http_session_vars)) && Z_TYPE_P(Z_REFVAL(PS(http_session_vars))) == IS_ARRAY)

#define SESSION_CHECK_ACTIVE_STATE	\
	if (PS(session_status) == php_session_active) {	\
		php_session_session_already_started_error(E_WARNING, "Session ini settings cannot be changed when a session is active");	\
		return FAILURE;	\
	}

#define SESSION_CHECK_OUTPUT_STATE										\
	if (SG(headers_sent) && stage != ZEND_INI_STAGE_DEACTIVATE) {												\
		php_session_headers_already_sent_error(E_WARNING, "Session ini settings cannot be changed after headers have already been sent");	\
		return FAILURE;													\
	}

#define SESSION_FORBIDDEN_CHARS "=,;.[ \t\r\n\013\014"
#define SESSION_FORBIDDEN_CHARS_FOR_ERROR_MSG "=,;.[ \\t\\r\\n\\013\\014"

#define APPLY_TRANS_SID (PS(use_trans_sid) && !PS(use_only_cookies))

static zend_result php_session_send_cookie(void);
static zend_result php_session_abort(void);

/* Initialized in MINIT, readonly otherwise. */
static int my_module_number = 0;

/* Dispatched by RINIT and by php_session_destroy */
static inline void php_rinit_session_globals(void)
{
	/* Do NOT init PS(mod_user_names) here! */
	/* TODO: These could be moved to MINIT and removed. These should be initialized by php_rshutdown_session_globals() always when execution is finished. */
	PS(id) = NULL;
	PS(session_status) = php_session_none;
	PS(in_save_handler) = 0;
	PS(set_handler) = 0;
	PS(mod_data) = NULL;
	PS(mod_user_is_open) = 0;
	PS(define_sid) = 1;
	PS(session_vars) = NULL;
	PS(module_number) = my_module_number;
	ZVAL_UNDEF(&PS(http_session_vars));
}

static inline void php_session_headers_already_sent_error(int severity, const char *message) {
	const char *output_start_filename = php_output_get_start_filename();
	int output_start_lineno = php_output_get_start_lineno();
	if (output_start_filename != NULL) {
		php_error_docref(NULL, severity, "%s (sent from %s on line %d)", message, output_start_filename, output_start_lineno);
	} else {
		php_error_docref(NULL, severity, "%s", message);
	}
}

static inline void php_session_session_already_started_error(int severity, const char *message) {
	if (PS(session_started_filename) != NULL) {
		php_error_docref(NULL, severity, "%s (started from %s on line %"PRIu32")", message, ZSTR_VAL(PS(session_started_filename)), PS(session_started_lineno));
	} else if (PS(auto_start)) {
		/* This option can't be changed at runtime, so we can assume it's because of this */
		php_error_docref(NULL, severity, "%s (session started automatically)", message);
	} else {
		php_error_docref(NULL, severity, "%s", message);
	}
}

static inline void php_session_cleanup_filename(void)
{
	if (PS(session_started_filename)) {
		zend_string_release(PS(session_started_filename));
		PS(session_started_filename) = NULL;
		PS(session_started_lineno) = 0;
	}
}

/* Dispatched by RSHUTDOWN and by php_session_destroy */
static void php_rshutdown_session_globals(void)
{
	/* Do NOT destroy PS(mod_user_names) here! */
	if (!Z_ISUNDEF(PS(http_session_vars))) {
		zval_ptr_dtor(&PS(http_session_vars));
		ZVAL_UNDEF(&PS(http_session_vars));
	}
	if (PS(mod_data) || PS(mod_user_implemented)) {
		zend_try {
			PS(mod)->s_close(&PS(mod_data));
		} zend_end_try();
	}
	if (PS(id)) {
		zend_string_release_ex(PS(id), 0);
		PS(id) = NULL;
	}

	if (PS(session_vars)) {
		zend_string_release_ex(PS(session_vars), 0);
		PS(session_vars) = NULL;
	}

	if (PS(mod_user_class_name)) {
		zend_string_release(PS(mod_user_class_name));
		PS(mod_user_class_name) = NULL;
	}

	php_session_cleanup_filename();

	/* User save handlers may end up directly here by misuse, bugs in user script, etc. */
	/* Set session status to prevent error while restoring save handler INI value. */
	PS(session_status) = php_session_none;
}

PHPAPI zend_result php_session_destroy(void)
{
	zend_result retval = SUCCESS;

	if (PS(session_status) != php_session_active) {
		php_error_docref(NULL, E_WARNING, "Trying to destroy uninitialized session");
		return FAILURE;
	}

	if (PS(id) && PS(mod)->s_destroy(&PS(mod_data), PS(id)) == FAILURE) {
		retval = FAILURE;
		if (!EG(exception)) {
			php_error_docref(NULL, E_WARNING, "Session object destruction failed");
		}
	}

	php_rshutdown_session_globals();
	php_rinit_session_globals();

	return retval;
}

PHPAPI void php_add_session_var(zend_string *name)
{
	IF_SESSION_VARS() {
		zval *sess_var = Z_REFVAL(PS(http_session_vars));
		SEPARATE_ARRAY(sess_var);
		if (!zend_hash_exists(Z_ARRVAL_P(sess_var), name)) {
			zval empty_var;
			ZVAL_NULL(&empty_var);
			zend_hash_update(Z_ARRVAL_P(sess_var), name, &empty_var);
		}
	}
}

PHPAPI zval* php_set_session_var(zend_string *name, zval *state_val, php_unserialize_data_t *var_hash)
{
	IF_SESSION_VARS() {
		zval *sess_var = Z_REFVAL(PS(http_session_vars));
		SEPARATE_ARRAY(sess_var);
		return zend_hash_update(Z_ARRVAL_P(sess_var), name, state_val);
	}
	return NULL;
}

PHPAPI zval* php_get_session_var(zend_string *name)
{
	IF_SESSION_VARS() {
		return zend_hash_find(Z_ARRVAL_P(Z_REFVAL(PS(http_session_vars))), name);
	}
	return NULL;
}

PHPAPI zval* php_get_session_var_str(const char *name, size_t name_len)
{
	IF_SESSION_VARS() {
		return zend_hash_str_find(Z_ARRVAL_P(Z_REFVAL(PS(http_session_vars))), name, name_len);
	}
	return NULL;
}

static void php_session_track_init(void)
{
	zval session_vars;
	zend_string *var_name = ZSTR_INIT_LITERAL("_SESSION", 0);
	/* Unconditionally destroy existing array -- possible dirty data */
	zend_delete_global_variable(var_name);

	if (!Z_ISUNDEF(PS(http_session_vars))) {
		zval_ptr_dtor(&PS(http_session_vars));
	}

	array_init(&session_vars);
	ZVAL_NEW_REF(&PS(http_session_vars), &session_vars);
	Z_ADDREF_P(&PS(http_session_vars));
	zend_hash_update_ind(&EG(symbol_table), var_name, &PS(http_session_vars));
	zend_string_release_ex(var_name, 0);
}

static zend_string *php_session_encode(void)
{
	IF_SESSION_VARS() {
        ZEND_ASSERT(PS(serializer));
		return PS(serializer)->encode();
	} else {
		php_error_docref(NULL, E_WARNING, "Cannot encode non-existent session");
	}
	return NULL;
}

static ZEND_COLD void php_session_cancel_decode(void)
{
	php_session_destroy();
	php_session_track_init();
	php_error_docref(NULL, E_WARNING, "Failed to decode session object. Session has been destroyed");
}

static zend_result php_session_decode(zend_string *data)
{
    ZEND_ASSERT(PS(serializer));
	zend_result result = SUCCESS;
	zend_try {
		if (PS(serializer)->decode(ZSTR_VAL(data), ZSTR_LEN(data)) == FAILURE) {
			php_session_cancel_decode();
			result = FAILURE;
		}
	} zend_catch {
		php_session_cancel_decode();
		zend_bailout();
	} zend_end_try();
	return result;
}

/*
 * Note that we cannot use the BASE64 alphabet here, because
 * it contains "/" and "+": both are unacceptable for simple inclusion
 * into URLs.
 */

static const char hexconvtab[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ,-";

static void bin_to_readable(unsigned char *in, size_t inlen, char *out, size_t outlen, char nbits)
{
	unsigned char *p, *q;
	unsigned short w;
	int mask;
	int have;

	p = (unsigned char *)in;
	q = (unsigned char *)in + inlen;

	w = 0;
	have = 0;
	mask = (1 << nbits) - 1;

	while (outlen--) {
		if (have < nbits) {
			if (p < q) {
				w |= *p++ << have;
				have += 8;
			} else {
				/* Should never happen. Input must be large enough. */
				ZEND_UNREACHABLE();
				break;
			}
		}

		/* consume nbits */
		*out++ = hexconvtab[w & mask];
		w >>= nbits;
		have -= nbits;
	}

	*out = '\0';
}

PHPAPI zend_string *php_session_create_id(PS_CREATE_SID_ARGS)
{
	unsigned char rbuf[PS_MAX_SID_LENGTH];
	zend_string *outid;

	/* It would be enough to read ceil(sid_length * sid_bits_per_character / 8) bytes here.
	 * We read sid_length bytes instead for simplicity. */
	if (php_random_bytes_throw(rbuf, PS(sid_length)) == FAILURE) {
		return NULL;
	}

	outid = zend_string_alloc(PS(sid_length), 0);
	bin_to_readable(
		rbuf, PS(sid_length),
		ZSTR_VAL(outid), ZSTR_LEN(outid),
		(char)PS(sid_bits_per_character));

	return outid;
}

/* Default session id char validation function allowed by ps_modules.
 * If you change the logic here, please also update the error message in
 * ps_modules appropriately */
PHPAPI zend_result php_session_valid_key(const char *key)
{
	size_t len;
	const char *p;
	char c;

	for (p = key; (c = *p); p++) {
		/* valid characters are [a-z], [A-Z], [0-9], - (hyphen) and , (comma) */
		if (!((c >= 'a' && c <= 'z')
				|| (c >= 'A' && c <= 'Z')
				|| (c >= '0' && c <= '9')
				|| c == ','
				|| c == '-')) {
			return FAILURE;
		}
	}

	len = p - key;

	/* Somewhat arbitrary length limit here, but should be way more than
	   anyone needs and avoids file-level warnings later on if we exceed MAX_PATH */
	if (len == 0 || len > PS_MAX_SID_LENGTH) {
		return FAILURE;
	}

	return SUCCESS;
}


static zend_long php_session_gc(bool immediate)
{
	zend_long num = -1;
	bool collect = immediate;

	/* GC must be done before reading session data. */
	if ((PS(mod_data) || PS(mod_user_implemented))) {
		if (!collect && PS(gc_probability) > 0) {
			collect = php_random_range(PS(random), 0, PS(gc_divisor) - 1) < PS(gc_probability);
		}

		if (collect) {
			PS(mod)->s_gc(&PS(mod_data), PS(gc_maxlifetime), &num);
		}
	}
	return num;
}

static zend_result php_session_initialize(void)
{
	zend_string *val = NULL;

	PS(session_status) = php_session_active;

	if (!PS(mod)) {
		PS(session_status) = php_session_disabled;
		php_error_docref(NULL, E_WARNING, "No storage module chosen - failed to initialize session");
		return FAILURE;
	}

	/* Open session handler first */
	if (PS(mod)->s_open(&PS(mod_data), ZSTR_VAL(PS(save_path)), ZSTR_VAL(PS(session_name))) == FAILURE
		/* || PS(mod_data) == NULL */ /* FIXME: open must set valid PS(mod_data) with success */
	) {
		php_session_abort();
		if (!EG(exception)) {
			php_error_docref(NULL, E_WARNING, "Failed to initialize storage module: %s (path: %s)", PS(mod)->s_name, ZSTR_VAL(PS(save_path)));
		}
		return FAILURE;
	}

	/* If there is no ID, use session module to create one */
	if (!PS(id) || !ZSTR_VAL(PS(id))[0]) {
		if (PS(id)) {
			zend_string_release_ex(PS(id), 0);
		}
		PS(id) = PS(mod)->s_create_sid(&PS(mod_data));
		if (!PS(id)) {
			php_session_abort();
			if (!EG(exception)) {
				zend_throw_error(NULL, "Failed to create session ID: %s (path: %s)", PS(mod)->s_name, ZSTR_VAL(PS(save_path)));
			}
			return FAILURE;
		}
		if (PS(use_cookies)) {
			PS(send_cookie) = 1;
		}
	} else if (PS(use_strict_mode) && PS(mod)->s_validate_sid &&
		PS(mod)->s_validate_sid(&PS(mod_data), PS(id)) == FAILURE
	) {
		if (PS(id)) {
			zend_string_release_ex(PS(id), 0);
		}
		PS(id) = PS(mod)->s_create_sid(&PS(mod_data));
		if (!PS(id)) {
			PS(id) = php_session_create_id(NULL);
		}
		if (PS(use_cookies)) {
			PS(send_cookie) = 1;
		}
	}

	if (php_session_reset_id() == FAILURE) {
		php_session_abort();
		return FAILURE;
	}

	/* Read data */
	php_session_track_init();
	if (PS(mod)->s_read(&PS(mod_data), PS(id), &val, PS(gc_maxlifetime)) == FAILURE) {
		php_session_abort();
		/* FYI: Some broken save handlers return FAILURE for non-existent session ID, this is incorrect */
		if (!EG(exception)) {
			php_error_docref(NULL, E_WARNING, "Failed to read session data: %s (path: %s)", PS(mod)->s_name, ZSTR_VAL(PS(save_path)));
		}
		return FAILURE;
	}

	/* GC must be done after read */
	php_session_gc(0);

	if (PS(session_vars)) {
		zend_string_release_ex(PS(session_vars), 0);
		PS(session_vars) = NULL;
	}
	if (val) {
		if (PS(lazy_write)) {
			PS(session_vars) = zend_string_copy(val);
		}
		php_session_decode(val);
		zend_string_release_ex(val, 0);
	}

	php_session_cleanup_filename();
	zend_string *session_started_filename = zend_get_executed_filename_ex();
	if (session_started_filename != NULL) {
		PS(session_started_filename) = zend_string_copy(session_started_filename);
		PS(session_started_lineno) = zend_get_executed_lineno();
	}
	return SUCCESS;
}

static void php_session_save_current_state(int write)
{
	zend_result ret = FAILURE;

	if (write) {
		IF_SESSION_VARS() {
			zend_string *handler_class_name = PS(mod_user_class_name);
			const char *handler_function_name = "write";

			if (PS(mod_data) || PS(mod_user_implemented)) {
				zend_string *val;

				val = php_session_encode();
				if (val) {
					if (PS(lazy_write) && PS(session_vars)
						&& PS(mod)->s_update_timestamp
						&& PS(mod)->s_update_timestamp != php_session_update_timestamp
						&& zend_string_equals(val, PS(session_vars))
					) {
						ret = PS(mod)->s_update_timestamp(&PS(mod_data), PS(id), val, PS(gc_maxlifetime));
						handler_function_name = handler_class_name != NULL ? "updateTimestamp" : "update_timestamp";
					} else {
						ret = PS(mod)->s_write(&PS(mod_data), PS(id), val, PS(gc_maxlifetime));
					}
					zend_string_release_ex(val, 0);
				} else {
					ret = PS(mod)->s_write(&PS(mod_data), PS(id), ZSTR_EMPTY_ALLOC(), PS(gc_maxlifetime));
				}
			}

			if ((ret == FAILURE) && !EG(exception)) {
				if (!PS(mod_user_implemented)) {
					php_error_docref(NULL, E_WARNING, "Failed to write session data (%s). Please "
									 "verify that the current setting of session.save_path "
									 "is correct (%s)",
									 PS(mod)->s_name,
									 ZSTR_VAL(PS(save_path)));
				} else if (handler_class_name != NULL) {
					php_error_docref(NULL, E_WARNING, "Failed to write session data using user "
									 "defined save handler. (session.save_path: %s, handler: %s::%s)", ZSTR_VAL(PS(save_path)),
									 ZSTR_VAL(handler_class_name), handler_function_name);
				} else {
					php_error_docref(NULL, E_WARNING, "Failed to write session data using user "
									 "defined save handler. (session.save_path: %s, handler: %s)", ZSTR_VAL(PS(save_path)),
									 handler_function_name);
				}
			}
		}
	}

	if (PS(mod_data) || PS(mod_user_implemented)) {
		PS(mod)->s_close(&PS(mod_data));
	}
}

static void php_session_normalize_vars(void)
{
	PS_ENCODE_VARS;

	IF_SESSION_VARS() {
		PS_ENCODE_LOOP(
			if (Z_TYPE_P(struc) == IS_PTR) {
				zval *zv = (zval *)Z_PTR_P(struc);
				ZVAL_COPY_VALUE(struc, zv);
				ZVAL_UNDEF(zv);
			}
		);
	}
}

/* *************************
   * INI Settings/Handlers *
   ************************* */

static PHP_INI_MH(OnUpdateSaveHandler)
{
	const ps_module *tmp;
	int err_type = E_ERROR;

	SESSION_CHECK_ACTIVE_STATE;
	SESSION_CHECK_OUTPUT_STATE;

	tmp = _php_find_ps_module(ZSTR_VAL(new_value));

	if (stage == ZEND_INI_STAGE_RUNTIME) {
		err_type = E_WARNING;
	}

	if (PG(modules_activated) && !tmp) {
		/* Do not output error when restoring ini options. */
		if (stage != ZEND_INI_STAGE_DEACTIVATE) {
			php_error_docref(NULL, err_type, "Session save handler \"%s\" cannot be found", ZSTR_VAL(new_value));
		}

		return FAILURE;
	}

	/* "user" save handler should not be set by user */
	if (!PS(set_handler) &&  tmp == ps_user_ptr) {
		php_error_docref(NULL, err_type, "Session save handler \"user\" cannot be set by ini_set()");
		return FAILURE;
	}

	PS(default_mod) = PS(mod);
	PS(mod) = tmp;

	return SUCCESS;
}

static PHP_INI_MH(OnUpdateSerializer)
{
	const ps_serializer *tmp;

	SESSION_CHECK_ACTIVE_STATE;
	SESSION_CHECK_OUTPUT_STATE;

	tmp = _php_find_ps_serializer(ZSTR_VAL(new_value));

	if (PG(modules_activated) && !tmp) {
		int err_type;

		if (stage == ZEND_INI_STAGE_RUNTIME) {
			err_type = E_WARNING;
		} else {
			err_type = E_ERROR;
		}

		/* Do not output error when restoring ini options. */
		if (stage != ZEND_INI_STAGE_DEACTIVATE) {
			php_error_docref(NULL, err_type, "Serialization handler \"%s\" cannot be found", ZSTR_VAL(new_value));
		}
		return FAILURE;
	}
	PS(serializer) = tmp;

	return SUCCESS;
}

static PHP_INI_MH(OnUpdateSaveDir)
{
	SESSION_CHECK_ACTIVE_STATE;
	SESSION_CHECK_OUTPUT_STATE;

	/* Only do the safemode/open_basedir check at runtime */
	if (stage == PHP_INI_STAGE_RUNTIME || stage == PHP_INI_STAGE_HTACCESS) {
		char *p;

		if (memchr(ZSTR_VAL(new_value), '\0', ZSTR_LEN(new_value)) != NULL) {
			return FAILURE;
		}

		/* we do not use zend_memrchr() since path can contain ; itself */
		if ((p = strchr(ZSTR_VAL(new_value), ';'))) {
			char *p2;
			p++;
			if ((p2 = strchr(p, ';'))) {
				p = p2 + 1;
			}
		} else {
			p = ZSTR_VAL(new_value);
		}

		if (PG(open_basedir) && *p && php_check_open_basedir(p)) {
			return FAILURE;
		}
	}

	return OnUpdateStr(entry, new_value, mh_arg1, mh_arg2, mh_arg3, stage);
}


static PHP_INI_MH(OnUpdateName)
{
	SESSION_CHECK_ACTIVE_STATE;
	SESSION_CHECK_OUTPUT_STATE;

	/* Numeric session.name won't work at all */
	if (
		ZSTR_LEN(new_value) == 0
		|| zend_str_has_nul_byte(new_value)
		|| is_numeric_str_function(new_value, NULL, NULL)
		|| strpbrk(ZSTR_VAL(new_value), SESSION_FORBIDDEN_CHARS) != NULL
	) {
		int err_type;

		if (stage == ZEND_INI_STAGE_RUNTIME || stage == ZEND_INI_STAGE_ACTIVATE || stage == ZEND_INI_STAGE_STARTUP) {
			err_type = E_WARNING;
		} else {
			err_type = E_ERROR;
		}

		/* Do not output error when restoring ini options. */
		if (stage != ZEND_INI_STAGE_DEACTIVATE) {
			php_error_docref(NULL, err_type, "session.name \"%s\" must not be numeric, empty, contain null bytes or any of the following characters \"" SESSION_FORBIDDEN_CHARS_FOR_ERROR_MSG "\"", ZSTR_VAL(new_value));
		}
		return FAILURE;
	}

	return OnUpdateStrNotEmpty(entry, new_value, mh_arg1, mh_arg2, mh_arg3, stage);
}


static PHP_INI_MH(OnUpdateCookieLifetime)
{
	SESSION_CHECK_ACTIVE_STATE;
	SESSION_CHECK_OUTPUT_STATE;

#ifdef ZEND_ENABLE_ZVAL_LONG64
	const zend_long maxcookie = ZEND_LONG_MAX - INT_MAX - 1;
#else
	const zend_long maxcookie = ZEND_LONG_MAX / 2 - 1;
#endif
	zend_long v = (zend_long)atol(ZSTR_VAL(new_value));
	if (v < 0) {
		php_error_docref(NULL, E_WARNING, "CookieLifetime cannot be negative");
		return FAILURE;
	} else if (v > maxcookie) {
		return SUCCESS;
	}
	return OnUpdateLongGEZero(entry, new_value, mh_arg1, mh_arg2, mh_arg3, stage);
}


static PHP_INI_MH(OnUpdateSessionLong)
{
	SESSION_CHECK_ACTIVE_STATE;
	SESSION_CHECK_OUTPUT_STATE;
	return OnUpdateLong(entry, new_value, mh_arg1, mh_arg2, mh_arg3, stage);
}

static PHP_INI_MH(OnUpdateSessionStr)
{
	SESSION_CHECK_ACTIVE_STATE;
	SESSION_CHECK_OUTPUT_STATE;
	return OnUpdateStr(entry, new_value, mh_arg1, mh_arg2, mh_arg3, stage);
}


static PHP_INI_MH(OnUpdateSessionBool)
{
	SESSION_CHECK_ACTIVE_STATE;
	SESSION_CHECK_OUTPUT_STATE;
	return OnUpdateBool(entry, new_value, mh_arg1, mh_arg2, mh_arg3, stage);
}


static PHP_INI_MH(OnUpdateSidLength)
{
	zend_long val;
	char *endptr = NULL;

	SESSION_CHECK_ACTIVE_STATE;
	SESSION_CHECK_OUTPUT_STATE;
	val = ZEND_STRTOL(ZSTR_VAL(new_value), &endptr, 10);
	if (val != 32) {
		php_error_docref("session.configuration", E_DEPRECATED, "session.sid_length INI setting is deprecated");
	}
	if (endptr && (*endptr == '\0')
		&& val >= 22 && val <= PS_MAX_SID_LENGTH) {
		/* Numeric value */
		PS(sid_length) = val;
		return SUCCESS;
	}

	php_error_docref(NULL, E_WARNING, "session.configuration \"session.sid_length\" must be between 22 and 256");
	return FAILURE;
}

static PHP_INI_MH(OnUpdateSidBits)
{
	zend_long val;
	char *endptr = NULL;

	SESSION_CHECK_ACTIVE_STATE;
	SESSION_CHECK_OUTPUT_STATE;
	val = ZEND_STRTOL(ZSTR_VAL(new_value), &endptr, 10);
	if (val != 4) {
		php_error_docref("session.configuration", E_DEPRECATED, "session.sid_bits_per_character INI setting is deprecated");
	}
	if (endptr && (*endptr == '\0')
		&& val >= 4 && val <=6) {
		/* Numeric value */
		PS(sid_bits_per_character) = val;
		return SUCCESS;
	}

	php_error_docref(NULL, E_WARNING, "session.configuration \"session.sid_bits_per_character\" must be between 4 and 6");
	return FAILURE;
}

static PHP_INI_MH(OnUpdateSessionGcProbability)
{
    SESSION_CHECK_ACTIVE_STATE;
    SESSION_CHECK_OUTPUT_STATE;

    zend_long tmp = zend_ini_parse_quantity_warn(new_value, entry->name);

    if (tmp < 0) {
        php_error_docref("session.gc_probability", E_WARNING, "session.gc_probability must be greater than or equal to 0");
        return FAILURE;
    }

    zend_long *p = (zend_long *) ZEND_INI_GET_ADDR();
    *p = tmp;

    return SUCCESS;
}

static PHP_INI_MH(OnUpdateSessionDivisor)
{
    SESSION_CHECK_ACTIVE_STATE;
    SESSION_CHECK_OUTPUT_STATE;

    zend_long tmp = zend_ini_parse_quantity_warn(new_value, entry->name);

    if (tmp <= 0) {
        php_error_docref("session.gc_divisor", E_WARNING, "session.gc_divisor must be greater than 0");
        return FAILURE;
    }

    zend_long *p = (zend_long *) ZEND_INI_GET_ADDR();
    *p = tmp;

    return SUCCESS;
}

static PHP_INI_MH(OnUpdateRfc1867Freq)
{
	int tmp = ZEND_ATOL(ZSTR_VAL(new_value));
	if(tmp < 0) {
		php_error_docref(NULL, E_WARNING, "session.upload_progress.freq must be greater than or equal to 0");
		return FAILURE;
	}
	if(ZSTR_LEN(new_value) > 0 && ZSTR_VAL(new_value)[ZSTR_LEN(new_value)-1] == '%') {
		if(tmp > 100) {
			php_error_docref(NULL, E_WARNING, "session.upload_progress.freq must be less than or equal to 100%%");
			return FAILURE;
		}
		PS(rfc1867_freq) = -tmp;
	} else {
		PS(rfc1867_freq) = tmp;
	}
	return SUCCESS;
}

static PHP_INI_MH(OnUpdateUseOnlyCookies)
{
	SESSION_CHECK_ACTIVE_STATE;
	SESSION_CHECK_OUTPUT_STATE;
	bool *p = (bool *) ZEND_INI_GET_ADDR();
	*p = zend_ini_parse_bool(new_value);
	if (!*p) {
		php_error_docref("session.configuration", E_DEPRECATED, "Disabling session.use_only_cookies INI setting is deprecated");
	}
	return SUCCESS;
}

static PHP_INI_MH(OnUpdateUseTransSid)
{
	SESSION_CHECK_ACTIVE_STATE;
	SESSION_CHECK_OUTPUT_STATE;
	bool *p = (bool *) ZEND_INI_GET_ADDR();
	*p = zend_ini_parse_bool(new_value);
	if (*p) {
		php_error_docref("session.configuration", E_DEPRECATED, "Enabling session.use_trans_sid INI setting is deprecated");
	}
	return SUCCESS;
}

static PHP_INI_MH(OnUpdateRefererCheck)
{
	SESSION_CHECK_ACTIVE_STATE;
	SESSION_CHECK_OUTPUT_STATE;
	if (ZSTR_LEN(new_value) != 0) {
		php_error_docref("session.configuration", E_DEPRECATED, "Usage of session.referer_check INI setting is deprecated");
	}
	return OnUpdateString(entry, new_value, mh_arg1, mh_arg2, mh_arg3, stage);
}

PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("session.save_path",          "",          PHP_INI_ALL, OnUpdateSaveDir,       save_path,          php_ps_globals,    ps_globals)
	STD_PHP_INI_ENTRY("session.name",               "PHPSESSID", PHP_INI_ALL, OnUpdateName,          session_name,       php_ps_globals,    ps_globals)
	PHP_INI_ENTRY("session.save_handler",           "files",     PHP_INI_ALL, OnUpdateSaveHandler)
	STD_PHP_INI_BOOLEAN("session.auto_start",       "0",         PHP_INI_PERDIR, OnUpdateBool,       auto_start,         php_ps_globals,    ps_globals)
	STD_PHP_INI_ENTRY("session.gc_probability",     "1",         PHP_INI_ALL, OnUpdateSessionGcProbability,    gc_probability,     php_ps_globals,    ps_globals)
	STD_PHP_INI_ENTRY("session.gc_divisor",         "100",       PHP_INI_ALL, OnUpdateSessionDivisor,gc_divisor,         php_ps_globals,    ps_globals)
	STD_PHP_INI_ENTRY("session.gc_maxlifetime",     "1440",      PHP_INI_ALL, OnUpdateSessionLong,          gc_maxlifetime,     php_ps_globals,    ps_globals)
	PHP_INI_ENTRY("session.serialize_handler",      "php",       PHP_INI_ALL, OnUpdateSerializer)
	STD_PHP_INI_ENTRY("session.cookie_lifetime",    "0",         PHP_INI_ALL, OnUpdateCookieLifetime,cookie_lifetime,    php_ps_globals,    ps_globals)
	STD_PHP_INI_ENTRY("session.cookie_path",        "/",         PHP_INI_ALL, OnUpdateSessionStr, cookie_path,        php_ps_globals,    ps_globals)
	STD_PHP_INI_ENTRY("session.cookie_domain",      "",          PHP_INI_ALL, OnUpdateSessionStr, cookie_domain,      php_ps_globals,    ps_globals)
	STD_PHP_INI_BOOLEAN("session.cookie_secure",    "0",         PHP_INI_ALL, OnUpdateSessionBool,   cookie_secure,      php_ps_globals,    ps_globals)
	STD_PHP_INI_BOOLEAN("session.cookie_httponly",  "0",         PHP_INI_ALL, OnUpdateSessionBool,   cookie_httponly,    php_ps_globals,    ps_globals)
	STD_PHP_INI_ENTRY("session.cookie_samesite",    "",          PHP_INI_ALL, OnUpdateSessionStr, cookie_samesite,    php_ps_globals,    ps_globals)
	STD_PHP_INI_BOOLEAN("session.use_cookies",      "1",         PHP_INI_ALL, OnUpdateSessionBool,   use_cookies,        php_ps_globals,    ps_globals)
	STD_PHP_INI_BOOLEAN("session.use_only_cookies", "1",         PHP_INI_ALL, OnUpdateUseOnlyCookies,   use_only_cookies,   php_ps_globals,    ps_globals)
	STD_PHP_INI_BOOLEAN("session.use_strict_mode",  "0",         PHP_INI_ALL, OnUpdateSessionBool,   use_strict_mode,    php_ps_globals,    ps_globals)
	STD_PHP_INI_ENTRY("session.referer_check",      "",          PHP_INI_ALL, OnUpdateRefererCheck, extern_referer_chk, php_ps_globals,    ps_globals)
	STD_PHP_INI_ENTRY("session.cache_limiter",      "nocache",   PHP_INI_ALL, OnUpdateSessionStr, cache_limiter,      php_ps_globals,    ps_globals)
	STD_PHP_INI_ENTRY("session.cache_expire",       "180",       PHP_INI_ALL, OnUpdateSessionLong,   cache_expire,       php_ps_globals,    ps_globals)
	STD_PHP_INI_BOOLEAN("session.use_trans_sid",    "0",         PHP_INI_ALL, OnUpdateUseTransSid,   use_trans_sid,      php_ps_globals,    ps_globals)
	PHP_INI_ENTRY("session.sid_length",             "32",        PHP_INI_ALL, OnUpdateSidLength)
	PHP_INI_ENTRY("session.sid_bits_per_character", "4",         PHP_INI_ALL, OnUpdateSidBits)
	STD_PHP_INI_BOOLEAN("session.lazy_write",       "1",         PHP_INI_ALL, OnUpdateSessionBool,    lazy_write,         php_ps_globals,    ps_globals)

	/* Upload progress */
	STD_PHP_INI_BOOLEAN("session.upload_progress.enabled",
	                                                "1",     ZEND_INI_PERDIR, OnUpdateBool,        rfc1867_enabled, php_ps_globals, ps_globals)
	STD_PHP_INI_BOOLEAN("session.upload_progress.cleanup",
	                                                "1",     ZEND_INI_PERDIR, OnUpdateBool,        rfc1867_cleanup, php_ps_globals, ps_globals)
	STD_PHP_INI_ENTRY("session.upload_progress.prefix",
	                                     "upload_progress_", ZEND_INI_PERDIR, OnUpdateStr,      rfc1867_prefix,  php_ps_globals, ps_globals)
	STD_PHP_INI_ENTRY("session.upload_progress.name",
	                          "PHP_SESSION_UPLOAD_PROGRESS", ZEND_INI_PERDIR, OnUpdateStr,      rfc1867_name,    php_ps_globals, ps_globals)
	STD_PHP_INI_ENTRY("session.upload_progress.freq",  "1%", ZEND_INI_PERDIR, OnUpdateRfc1867Freq, rfc1867_freq,    php_ps_globals, ps_globals)
	STD_PHP_INI_ENTRY("session.upload_progress.min_freq",
	                                                   "1",  ZEND_INI_PERDIR, OnUpdateReal,        rfc1867_min_freq,php_ps_globals, ps_globals)

	/* Commented out until future discussion */
	/* PHP_INI_ENTRY("session.encode_sources", "globals,track", PHP_INI_ALL, NULL) */
PHP_INI_END()

/* ***************
   * Serializers *
   *************** */
PS_SERIALIZER_ENCODE_FUNC(php_serialize)
{
	smart_str buf = {0};
	php_serialize_data_t var_hash;

	IF_SESSION_VARS() {
		PHP_VAR_SERIALIZE_INIT(var_hash);
		php_var_serialize(&buf, Z_REFVAL(PS(http_session_vars)), &var_hash);
		PHP_VAR_SERIALIZE_DESTROY(var_hash);
	}
	return buf.s;
}

PS_SERIALIZER_DECODE_FUNC(php_serialize)
{
	const char *endptr = val + vallen;
	zval session_vars;
	php_unserialize_data_t var_hash;
	bool result;
	zend_string *var_name = ZSTR_INIT_LITERAL("_SESSION", 0);

	ZVAL_NULL(&session_vars);
	PHP_VAR_UNSERIALIZE_INIT(var_hash);
	result = php_var_unserialize(
		&session_vars, (const unsigned char **)&val, (const unsigned char *)endptr, &var_hash);
	PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
	if (!result) {
		zval_ptr_dtor(&session_vars);
		ZVAL_NULL(&session_vars);
	}

	if (!Z_ISUNDEF(PS(http_session_vars))) {
		zval_ptr_dtor(&PS(http_session_vars));
	}
	if (Z_TYPE(session_vars) == IS_NULL) {
		array_init(&session_vars);
	}
	ZVAL_NEW_REF(&PS(http_session_vars), &session_vars);
	Z_ADDREF_P(&PS(http_session_vars));
	zend_hash_update_ind(&EG(symbol_table), var_name, &PS(http_session_vars));
	zend_string_release_ex(var_name, 0);
	return result || !vallen ? SUCCESS : FAILURE;
}

#define PS_BIN_NR_OF_BITS 8
#define PS_BIN_UNDEF (1<<(PS_BIN_NR_OF_BITS-1))
#define PS_BIN_MAX (PS_BIN_UNDEF-1)

PS_SERIALIZER_ENCODE_FUNC(php_binary)
{
	smart_str buf = {0};
	php_serialize_data_t var_hash;
	PS_ENCODE_VARS;

	PHP_VAR_SERIALIZE_INIT(var_hash);

	PS_ENCODE_LOOP(
			if (ZSTR_LEN(key) > PS_BIN_MAX) continue;
			smart_str_appendc(&buf, (unsigned char)ZSTR_LEN(key));
			smart_str_append(&buf, key);
			php_var_serialize(&buf, struc, &var_hash);
	);

	smart_str_0(&buf);
	PHP_VAR_SERIALIZE_DESTROY(var_hash);

	return buf.s;
}

PS_SERIALIZER_DECODE_FUNC(php_binary)
{
	const char *p;
	const char *endptr = val + vallen;
	zend_string *name;
	php_unserialize_data_t var_hash;
	zval *current, rv;

	PHP_VAR_UNSERIALIZE_INIT(var_hash);

	for (p = val; p < endptr; ) {
		size_t namelen = ((unsigned char)(*p)) & (~PS_BIN_UNDEF);

		if (namelen > PS_BIN_MAX || (p + namelen) >= endptr) {
			PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
			return FAILURE;
		}

		name = zend_string_init(p + 1, namelen, 0);
		p += namelen + 1;
		current = var_tmp_var(&var_hash);

		if (php_var_unserialize(current, (const unsigned char **) &p, (const unsigned char *) endptr, &var_hash)) {
			ZVAL_PTR(&rv, current);
			php_set_session_var(name, &rv, &var_hash);
		} else {
			zend_string_release_ex(name, 0);
			php_session_normalize_vars();
			PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
			return FAILURE;
		}
		zend_string_release_ex(name, 0);
	}

	php_session_normalize_vars();
	PHP_VAR_UNSERIALIZE_DESTROY(var_hash);

	return SUCCESS;
}

#define PS_DELIMITER '|'

PS_SERIALIZER_ENCODE_FUNC(php)
{
	smart_str buf = {0};
	php_serialize_data_t var_hash;
	bool fail = false;
	PS_ENCODE_VARS;

	PHP_VAR_SERIALIZE_INIT(var_hash);

	PS_ENCODE_LOOP(
		smart_str_append(&buf, key);
		if (memchr(ZSTR_VAL(key), PS_DELIMITER, ZSTR_LEN(key))) {
			PHP_VAR_SERIALIZE_DESTROY(var_hash);
			smart_str_free(&buf);
			fail = true;
			php_error_docref(NULL, E_WARNING, "Failed to write session data. Data contains invalid key \"%s\"", ZSTR_VAL(key));
			break;
		}
		smart_str_appendc(&buf, PS_DELIMITER);
		php_var_serialize(&buf, struc, &var_hash);
	);

	if (fail) {
		return NULL;
	}

	smart_str_0(&buf);

	PHP_VAR_SERIALIZE_DESTROY(var_hash);
	return buf.s;
}

PS_SERIALIZER_DECODE_FUNC(php)
{
	const char *p, *q;
	const char *endptr = val + vallen;
	ptrdiff_t namelen;
	zend_string *name;
	zend_result retval = SUCCESS;
	php_unserialize_data_t var_hash;
	zval *current, rv;

	PHP_VAR_UNSERIALIZE_INIT(var_hash);

	p = val;

	while (p < endptr) {
		q = p;
		while (*q != PS_DELIMITER) {
			if (++q >= endptr) {
				retval = FAILURE;
				goto break_outer_loop;
			}
		}

		namelen = q - p;
		name = zend_string_init(p, namelen, 0);
		q++;

		current = var_tmp_var(&var_hash);
		if (php_var_unserialize(current, (const unsigned char **)&q, (const unsigned char *)endptr, &var_hash)) {
			ZVAL_PTR(&rv, current);
			php_set_session_var(name, &rv, &var_hash);
		} else {
			zend_string_release_ex(name, 0);
			retval = FAILURE;
			goto break_outer_loop;
		}
		zend_string_release_ex(name, 0);
		p = q;
	}

break_outer_loop:
	php_session_normalize_vars();

	PHP_VAR_UNSERIALIZE_DESTROY(var_hash);

	return retval;
}

#define MAX_SERIALIZERS 32
#define PREDEFINED_SERIALIZERS 3

static ps_serializer ps_serializers[MAX_SERIALIZERS + 1] = {
	PS_SERIALIZER_ENTRY(php_serialize),
	PS_SERIALIZER_ENTRY(php),
	PS_SERIALIZER_ENTRY(php_binary)
};

PHPAPI zend_result php_session_register_serializer(const char *name, zend_string *(*encode)(PS_SERIALIZER_ENCODE_ARGS), zend_result (*decode)(PS_SERIALIZER_DECODE_ARGS))
{
	zend_result ret = FAILURE;

	for (int i = 0; i < MAX_SERIALIZERS; i++) {
		if (ps_serializers[i].name == NULL) {
			ps_serializers[i].name = name;
			ps_serializers[i].encode = encode;
			ps_serializers[i].decode = decode;
			ps_serializers[i + 1].name = NULL;
			ret = SUCCESS;
			break;
		}
	}
	return ret;
}

/* *******************
   * Storage Modules *
   ******************* */

#define MAX_MODULES 32
#define PREDEFINED_MODULES 2

static const ps_module *ps_modules[MAX_MODULES + 1] = {
	ps_files_ptr,
	ps_user_ptr
};

PHPAPI zend_result php_session_register_module(const ps_module *ptr)
{
	zend_result ret = FAILURE;

	for (int i = 0; i < MAX_MODULES; i++) {
		if (!ps_modules[i]) {
			ps_modules[i] = ptr;
			ret = SUCCESS;
			break;
		}
	}
	return ret;
}

/* Dummy PS module function */
/* We consider any ID valid (thus also implying that a session with such an ID exists),
	thus we always return SUCCESS */
PHPAPI zend_result php_session_validate_sid(PS_VALIDATE_SID_ARGS) {
	return SUCCESS;
}

/* Dummy PS module function */
PHPAPI zend_result php_session_update_timestamp(PS_UPDATE_TIMESTAMP_ARGS) {
	return SUCCESS;
}


/* ******************
   * Cache Limiters *
   ****************** */

typedef struct {
	char *name;
	void (*func)(void);
} php_session_cache_limiter_t;

#define CACHE_LIMITER(name) _php_cache_limiter_##name
#define CACHE_LIMITER_FUNC(name) static void CACHE_LIMITER(name)(void)
#define CACHE_LIMITER_ENTRY(name) { #name, CACHE_LIMITER(name) },
#define ADD_HEADER(a) sapi_add_header(a, strlen(a), 1);
#define MAX_STR 512

static const char *month_names[] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *week_days[] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
};

static inline void strcpy_gmt(char *ubuf, const time_t *when)
{
	char buf[MAX_STR];
	struct tm tm, *res;
	int n;

	res = php_gmtime_r(when, &tm);

	if (!res) {
		ubuf[0] = '\0';
		return;
	}

	n = slprintf(buf, sizeof(buf), "%s, %02d %s %d %02d:%02d:%02d GMT", /* SAFE */
				week_days[tm.tm_wday], tm.tm_mday,
				month_names[tm.tm_mon], tm.tm_year + 1900,
				tm.tm_hour, tm.tm_min,
				tm.tm_sec);
	memcpy(ubuf, buf, n);
	ubuf[n] = '\0';
}

static inline void last_modified(void)
{
	const char *path;
	zend_stat_t sb = {0};
	char buf[MAX_STR + 1];

	path = SG(request_info).path_translated;
	if (path) {
		if (VCWD_STAT(path, &sb) == -1) {
			return;
		}

#define LAST_MODIFIED "Last-Modified: "
		memcpy(buf, LAST_MODIFIED, sizeof(LAST_MODIFIED) - 1);
		strcpy_gmt(buf + sizeof(LAST_MODIFIED) - 1, &sb.st_mtime);
		ADD_HEADER(buf);
	}
}

#define EXPIRES "Expires: "
CACHE_LIMITER_FUNC(public)
{
	char buf[MAX_STR + 1];
	struct timeval tv;
	time_t now;

	gettimeofday(&tv, NULL);
	now = tv.tv_sec + PS(cache_expire) * 60;
	memcpy(buf, EXPIRES, sizeof(EXPIRES) - 1);
	strcpy_gmt(buf + sizeof(EXPIRES) - 1, &now);
	ADD_HEADER(buf);

	snprintf(buf, sizeof(buf) , "Cache-Control: public, max-age=" ZEND_LONG_FMT, PS(cache_expire) * 60); /* SAFE */
	ADD_HEADER(buf);

	last_modified();
}

CACHE_LIMITER_FUNC(private_no_expire)
{
	char buf[MAX_STR + 1];

	snprintf(buf, sizeof(buf), "Cache-Control: private, max-age=" ZEND_LONG_FMT, PS(cache_expire) * 60); /* SAFE */
	ADD_HEADER(buf);

	last_modified();
}

CACHE_LIMITER_FUNC(private)
{
	ADD_HEADER("Expires: Thu, 19 Nov 1981 08:52:00 GMT");
	CACHE_LIMITER(private_no_expire)();
}

CACHE_LIMITER_FUNC(nocache)
{
	ADD_HEADER("Expires: Thu, 19 Nov 1981 08:52:00 GMT");

	/* For HTTP/1.1 conforming clients */
	ADD_HEADER("Cache-Control: no-store, no-cache, must-revalidate");

	/* For HTTP/1.0 conforming clients */
	ADD_HEADER("Pragma: no-cache");
}

static const php_session_cache_limiter_t php_session_cache_limiters[] = {
	CACHE_LIMITER_ENTRY(public)
	CACHE_LIMITER_ENTRY(private)
	CACHE_LIMITER_ENTRY(private_no_expire)
	CACHE_LIMITER_ENTRY(nocache)
	{0}
};

static int php_session_cache_limiter(void)
{
	const php_session_cache_limiter_t *lim;

	if (ZSTR_LEN(PS(cache_limiter)) == 0) {
		return 0;
	}
	if (PS(session_status) != php_session_active) return -1;

	if (SG(headers_sent)) {
		php_session_abort();
		php_session_headers_already_sent_error(E_WARNING, "Session cache limiter cannot be sent after headers have already been sent");
		return -2;
	}

	for (lim = php_session_cache_limiters; lim->name; lim++) {
		if (!strcasecmp(lim->name, ZSTR_VAL(PS(cache_limiter)))) {
			lim->func();
			return 0;
		}
	}

	return -1;
}

/* *********************
   * Cookie Management *
   ********************* */

/*
 * Remove already sent session ID cookie.
 * It must be directly removed from SG(sapi_header) because sapi_add_header_ex()
 * removes all of matching cookie. i.e. It deletes all of Set-Cookie headers.
 */
static void php_session_remove_cookie(void) {
	sapi_header_struct *header;
	zend_llist *l = &SG(sapi_headers).headers;
	zend_llist_element *next;
	zend_llist_element *current;
	char *session_cookie;
	size_t session_cookie_len;
	size_t len = sizeof("Set-Cookie")-1;

	ZEND_ASSERT(strpbrk(ZSTR_VAL(PS(session_name)), SESSION_FORBIDDEN_CHARS) == NULL);
	session_cookie_len = spprintf(&session_cookie, 0, "Set-Cookie: %s=", ZSTR_VAL(PS(session_name)));

	current = l->head;
	while (current) {
		header = (sapi_header_struct *)(current->data);
		next = current->next;
		if (header->header_len > len && header->header[len] == ':'
			&& !strncmp(header->header, session_cookie, session_cookie_len)) {
			if (current->prev) {
				current->prev->next = next;
			} else {
				l->head = next;
			}
			if (next) {
				next->prev = current->prev;
			} else {
				l->tail = current->prev;
			}
			sapi_free_header(header);
			efree(current);
			--l->count;
		}
		current = next;
	}
	efree(session_cookie);
}

static zend_result php_session_send_cookie(void)
{
	smart_str ncookie = {0};
	zend_string *date_fmt = NULL;
	zend_string *e_id;

	if (SG(headers_sent)) {
		php_session_headers_already_sent_error(E_WARNING, "Session cookie cannot be sent after headers have already been sent");
		return FAILURE;
	}

	ZEND_ASSERT(strpbrk(ZSTR_VAL(PS(session_name)), SESSION_FORBIDDEN_CHARS) == NULL);

	/* URL encode id because it might be user supplied */
	e_id = php_url_encode(ZSTR_VAL(PS(id)), ZSTR_LEN(PS(id)));

	smart_str_appends(&ncookie, "Set-Cookie: ");
	smart_str_append(&ncookie, PS(session_name));
	smart_str_appendc(&ncookie, '=');
	smart_str_append(&ncookie, e_id);

	zend_string_release_ex(e_id, 0);

	if (PS(cookie_lifetime) > 0) {
		struct timeval tv;
		time_t t;

		gettimeofday(&tv, NULL);
		t = tv.tv_sec + PS(cookie_lifetime);

		if (t > 0) {
			date_fmt = php_format_date(ZEND_STRL("D, d M Y H:i:s \\G\\M\\T"), t, false);
			smart_str_appends(&ncookie, COOKIE_EXPIRES);
			smart_str_append(&ncookie, date_fmt);
			zend_string_release_ex(date_fmt, 0);

			smart_str_appends(&ncookie, COOKIE_MAX_AGE);
			smart_str_append_long(&ncookie, PS(cookie_lifetime));
		}
	}

	if (ZSTR_LEN(PS(cookie_path))) {
		smart_str_appends(&ncookie, COOKIE_PATH);
		smart_str_append(&ncookie, PS(cookie_path));
	}

	if (ZSTR_LEN(PS(cookie_domain))) {
		smart_str_appends(&ncookie, COOKIE_DOMAIN);
		smart_str_append(&ncookie, PS(cookie_domain));
	}

	if (PS(cookie_secure)) {
		smart_str_appends(&ncookie, COOKIE_SECURE);
	}

	if (PS(cookie_httponly)) {
		smart_str_appends(&ncookie, COOKIE_HTTPONLY);
	}

	if (ZSTR_LEN(PS(cookie_samesite))) {
		smart_str_appends(&ncookie, COOKIE_SAMESITE);
		smart_str_append(&ncookie, PS(cookie_samesite));
	}

	smart_str_0(&ncookie);

	php_session_remove_cookie(); /* remove already sent session ID cookie */
	/*	'replace' must be 0 here, else a previous Set-Cookie
		header, probably sent with setcookie() will be replaced! */
	sapi_add_header_ex(estrndup(ZSTR_VAL(ncookie.s), ZSTR_LEN(ncookie.s)), ZSTR_LEN(ncookie.s), 0, 0);
	smart_str_free(&ncookie);

	return SUCCESS;
}

PHPAPI const ps_module *_php_find_ps_module(const char *name)
{
	const ps_module *ret = NULL;
	const ps_module **mod;
	int i;

	for (i = 0, mod = ps_modules; i < MAX_MODULES; i++, mod++) {
		if (*mod && !strcasecmp(name, (*mod)->s_name)) {
			ret = *mod;
			break;
		}
	}
	return ret;
}

PHPAPI const ps_serializer *_php_find_ps_serializer(const char *name)
{
	const ps_serializer *ret = NULL;
	const ps_serializer *mod;

	for (mod = ps_serializers; mod->name; mod++) {
		if (!strcasecmp(name, mod->name)) {
			ret = mod;
			break;
		}
	}
	return ret;
}

static void ppid2sid(zval *ppid) {
	ZVAL_DEREF(ppid);
	if (Z_TYPE_P(ppid) == IS_STRING) {
		PS(id) = zend_string_copy(Z_STR_P(ppid));
		PS(send_cookie) = 0;
	} else {
		PS(id) = NULL;
		PS(send_cookie) = 1;
	}
}


PHPAPI zend_result php_session_reset_id(void)
{
	int module_number = PS(module_number);
	zval *sid, *data, *ppid;
	bool apply_trans_sid;

	if (!PS(id)) {
		php_error_docref(NULL, E_WARNING, "Cannot set session ID - session ID is not initialized");
		return FAILURE;
	}

	if (PS(use_cookies) && PS(send_cookie)) {
		zend_result cookies_sent = php_session_send_cookie();
		if (UNEXPECTED(cookies_sent == FAILURE)) {
			return FAILURE;
		}
		PS(send_cookie) = 0;
	}

	/* If the SID constant exists, destroy it. */
	/* We must not delete any items in EG(zend_constants) */
	/* zend_hash_str_del(EG(zend_constants), ZEND_STRL("sid")); */
	sid = zend_get_constant_str(ZEND_STRL("SID"));

	if (PS(define_sid)) {
		smart_str var = {0};

		smart_str_append(&var, PS(session_name));
		smart_str_appendc(&var, '=');
		smart_str_append(&var, PS(id));
		smart_str_0(&var);
		if (sid) {
			zval_ptr_dtor(sid);
			ZVAL_STR(sid, smart_str_extract(&var));
		} else {
			REGISTER_STRINGL_CONSTANT("SID", ZSTR_VAL(var.s), ZSTR_LEN(var.s),  CONST_DEPRECATED);
			smart_str_free(&var);
		}
	} else {
		if (sid) {
			zval_ptr_dtor(sid);
			ZVAL_EMPTY_STRING(sid);
		} else {
			REGISTER_STRINGL_CONSTANT("SID", "", 0, CONST_DEPRECATED);
		}
	}

	/* Apply trans sid if sid cookie is not set */
	apply_trans_sid = 0;
	if (APPLY_TRANS_SID) {
		apply_trans_sid = 1;
		if (PS(use_cookies) &&
			(data = zend_hash_str_find(&EG(symbol_table), ZEND_STRL("_COOKIE")))) {
			ZVAL_DEREF(data);
			if (Z_TYPE_P(data) == IS_ARRAY &&
				(ppid = zend_hash_find(Z_ARRVAL_P(data), PS(session_name)))) {
				ZVAL_DEREF(ppid);
				apply_trans_sid = 0;
			}
		}
	}
	if (apply_trans_sid) {
		php_url_scanner_reset_session_var(PS(session_name), true); /* This may fail when session name has changed */
		php_url_scanner_add_session_var(ZSTR_VAL(PS(session_name)), ZSTR_LEN(PS(session_name)), ZSTR_VAL(PS(id)), ZSTR_LEN(PS(id)), true);
	}
	return SUCCESS;
}


PHPAPI zend_result php_session_start(void)
{
	zval *ppid;
	zval *data;
	char *value;

	switch (PS(session_status)) {
		case php_session_active:
			php_session_session_already_started_error(E_NOTICE, "Ignoring session_start() because a session has already been started");
			return FAILURE;
			break;

		case php_session_disabled:
			value = zend_ini_string(ZEND_STRL("session.save_handler"), false);
			if (!PS(mod) && value) {
				PS(mod) = _php_find_ps_module(value);
				if (!PS(mod)) {
					php_error_docref(NULL, E_WARNING, "Cannot find session save handler \"%s\" - session startup failed", value);
					return FAILURE;
				}
			}
			value = zend_ini_string(ZEND_STRL("session.serialize_handler"), false);
			if (!PS(serializer) && value) {
				PS(serializer) = _php_find_ps_serializer(value);
				if (!PS(serializer)) {
					php_error_docref(NULL, E_WARNING, "Cannot find session serialization handler \"%s\" - session startup failed", value);
					return FAILURE;
				}
			}
			PS(session_status) = php_session_none;
			ZEND_FALLTHROUGH;

		case php_session_none:
		default:
			/* Setup internal flags */
			PS(define_sid) = !PS(use_only_cookies); /* SID constant is defined when non-cookie ID is used */
			PS(send_cookie) = PS(use_cookies) || PS(use_only_cookies);
	}

	/*
	 * Cookies are preferred, because initially cookie and get
	 * variables will be available.
	 * URL/POST session ID may be used when use_only_cookies=Off.
	 * session.use_strice_mode=On prevents session adoption.
	 * Session based file upload progress uses non-cookie ID.
	 */

	if (!PS(id)) {
		if (PS(use_cookies) && (data = zend_hash_str_find(&EG(symbol_table), ZEND_STRL("_COOKIE")))) {
			ZVAL_DEREF(data);
			if (Z_TYPE_P(data) == IS_ARRAY && (ppid = zend_hash_find(Z_ARRVAL_P(data), PS(session_name)))) {
				ppid2sid(ppid);
				PS(send_cookie) = 0;
				PS(define_sid) = 0;
			}
		}
		/* Initialize session ID from non cookie values */
		if (!PS(use_only_cookies)) {
			if (!PS(id) && (data = zend_hash_str_find(&EG(symbol_table), ZEND_STRL("_GET")))) {
				ZVAL_DEREF(data);
				if (Z_TYPE_P(data) == IS_ARRAY && (ppid = zend_hash_find(Z_ARRVAL_P(data), PS(session_name)))) {
					ppid2sid(ppid);
				}
			}
			if (!PS(id) && (data = zend_hash_str_find(&EG(symbol_table), ZEND_STRL("_POST")))) {
				ZVAL_DEREF(data);
				if (Z_TYPE_P(data) == IS_ARRAY && (ppid = zend_hash_find(Z_ARRVAL_P(data), PS(session_name)))) {
					ppid2sid(ppid);
				}
			}
			/* Check whether the current request was referred to by
			 * an external site which invalidates the previously found id. */
			if (PS(id) && PS(extern_referer_chk)[0] != '\0' &&
				!Z_ISUNDEF(PG(http_globals)[TRACK_VARS_SERVER]) &&
				(data = zend_hash_str_find(Z_ARRVAL(PG(http_globals)[TRACK_VARS_SERVER]), ZEND_STRL("HTTP_REFERER"))) &&
				Z_TYPE_P(data) == IS_STRING &&
				Z_STRLEN_P(data) != 0 &&
				strstr(Z_STRVAL_P(data), PS(extern_referer_chk)) == NULL
			) {
				zend_string_release_ex(PS(id), 0);
				PS(id) = NULL;
			}
		}
	}

	/* Finally check session id for dangerous characters
	 * Security note: session id may be embedded in HTML pages.*/
	if (PS(id) && strpbrk(ZSTR_VAL(PS(id)), "\r\n\t <>'\"\\")) {
		zend_string_release_ex(PS(id), 0);
		PS(id) = NULL;
	}

	if (php_session_initialize() == FAILURE
		|| php_session_cache_limiter() == -2) {
		PS(session_status) = php_session_none;
		if (PS(id)) {
			zend_string_release_ex(PS(id), 0);
			PS(id) = NULL;
		}
		return FAILURE;
	}

	return SUCCESS;
}

PHPAPI zend_result php_session_flush(int write)
{
	if (PS(session_status) == php_session_active) {
		php_session_save_current_state(write);
		PS(session_status) = php_session_none;
		return SUCCESS;
	}
	return FAILURE;
}

PHPAPI php_session_status php_get_session_status(void)
{
	return PS(session_status);
}

static zend_result php_session_abort(void)
{
	if (PS(session_status) == php_session_active) {
		if (PS(mod_data) || PS(mod_user_implemented)) {
			PS(mod)->s_close(&PS(mod_data));
		}
		PS(session_status) = php_session_none;
		return SUCCESS;
	}
	return FAILURE;
}

static zend_result php_session_reset(void)
{
	if (PS(session_status) == php_session_active
		&& php_session_initialize() == SUCCESS) {
		return SUCCESS;
	}
	return FAILURE;
}


/* This API is not used by any PHP modules including session currently.
   session_adapt_url() may be used to set Session ID to target url without
   starting "URL-Rewriter" output handler. */
PHPAPI void session_adapt_url(const char *url, size_t url_len, char **new_url, size_t *new_len)
{
	if (APPLY_TRANS_SID && (PS(session_status) == php_session_active)) {
		*new_url = php_url_scanner_adapt_single_url(url, url_len, ZSTR_VAL(PS(session_name)), ZSTR_VAL(PS(id)), new_len, true);
	}
}

/* ********************************
   * Userspace exported functions *
   ******************************** */

PHP_FUNCTION(session_set_cookie_params)
{
	HashTable *options_ht;
	zend_long lifetime_long;
	zend_string *lifetime = NULL, *path = NULL, *domain = NULL, *samesite = NULL;
	bool secure = 0, secure_null = 1;
	bool httponly = 0, httponly_null = 1;
	zend_string *ini_name;
	zend_result result;
	int found = 0;

	ZEND_PARSE_PARAMETERS_START(1, 5)
		Z_PARAM_ARRAY_HT_OR_LONG(options_ht, lifetime_long)
		Z_PARAM_OPTIONAL
		Z_PARAM_STR_OR_NULL(path)
		Z_PARAM_STR_OR_NULL(domain)
		Z_PARAM_BOOL_OR_NULL(secure, secure_null)
		Z_PARAM_BOOL_OR_NULL(httponly, httponly_null)
	ZEND_PARSE_PARAMETERS_END();

	if (!PS(use_cookies)) {
		php_error_docref(NULL, E_WARNING, "Session cookies cannot be used when session.use_cookies is disabled");
		RETURN_FALSE;
	}

	if (PS(session_status) == php_session_active) {
		php_session_session_already_started_error(E_WARNING, "Session cookie parameters cannot be changed when a session is active");
		RETURN_FALSE;
	}

	if (SG(headers_sent)) {
		php_session_headers_already_sent_error(E_WARNING, "Session cookie parameters cannot be changed after headers have already been sent");
		RETURN_FALSE;
	}

	if (options_ht) {
		zend_string *key;
		zval *value;

		if (path) {
			zend_argument_value_error(2, "must be null when argument #1 ($lifetime_or_options) is an array");
			RETURN_THROWS();
		}

		if (domain) {
			zend_argument_value_error(3, "must be null when argument #1 ($lifetime_or_options) is an array");
			RETURN_THROWS();
		}

		if (!secure_null) {
			zend_argument_value_error(4, "must be null when argument #1 ($lifetime_or_options) is an array");
			RETURN_THROWS();
		}

		if (!httponly_null) {
			zend_argument_value_error(5, "must be null when argument #1 ($lifetime_or_options) is an array");
			RETURN_THROWS();
		}
		ZEND_HASH_FOREACH_STR_KEY_VAL(options_ht, key, value) {
			if (key) {
				ZVAL_DEREF(value);
				if (zend_string_equals_literal_ci(key, "lifetime")) {
					lifetime = zval_get_string(value);
					found++;
				} else if (zend_string_equals_literal_ci(key, "path")) {
					path = zval_get_string(value);
					found++;
				} else if (zend_string_equals_literal_ci(key, "domain")) {
					domain = zval_get_string(value);
					found++;
				} else if (zend_string_equals_literal_ci(key, "secure")) {
					secure = zval_is_true(value);
					secure_null = 0;
					found++;
				} else if (zend_string_equals_literal_ci(key, "httponly")) {
					httponly = zval_is_true(value);
					httponly_null = 0;
					found++;
				} else if (zend_string_equals_literal_ci(key, "samesite")) {
					samesite = zval_get_string(value);
					found++;
				} else {
					php_error_docref(NULL, E_WARNING, "Argument #1 ($lifetime_or_options) contains an unrecognized key \"%s\"", ZSTR_VAL(key));
				}
			} else {
				php_error_docref(NULL, E_WARNING, "Argument #1 ($lifetime_or_options) cannot contain numeric keys");
			}
		} ZEND_HASH_FOREACH_END();

		if (found == 0) {
			zend_argument_value_error(1, "must contain at least 1 valid key");
			RETURN_THROWS();
		}
	} else {
		lifetime = zend_long_to_str(lifetime_long);
	}

	/* Exception during string conversion */
	if (EG(exception)) {
		goto cleanup;
	}

	if (lifetime) {
		ini_name = ZSTR_INIT_LITERAL("session.cookie_lifetime", 0);
		result = zend_alter_ini_entry(ini_name, lifetime, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
		zend_string_release_ex(ini_name, 0);
		if (result == FAILURE) {
			RETVAL_FALSE;
			goto cleanup;
		}
	}
	if (path) {
		ini_name = ZSTR_INIT_LITERAL("session.cookie_path", 0);
		result = zend_alter_ini_entry(ini_name, path, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
		zend_string_release_ex(ini_name, 0);
		if (result == FAILURE) {
			RETVAL_FALSE;
			goto cleanup;
		}
	}
	if (domain) {
		ini_name = ZSTR_INIT_LITERAL("session.cookie_domain", 0);
		result = zend_alter_ini_entry(ini_name, domain, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
		zend_string_release_ex(ini_name, 0);
		if (result == FAILURE) {
			RETVAL_FALSE;
			goto cleanup;
		}
	}
	if (!secure_null) {
		ini_name = ZSTR_INIT_LITERAL("session.cookie_secure", 0);
		result = zend_alter_ini_entry_chars(ini_name, secure ? "1" : "0", 1, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
		zend_string_release_ex(ini_name, 0);
		if (result == FAILURE) {
			RETVAL_FALSE;
			goto cleanup;
		}
	}
	if (!httponly_null) {
		ini_name = ZSTR_INIT_LITERAL("session.cookie_httponly", 0);
		result = zend_alter_ini_entry_chars(ini_name, httponly ? "1" : "0", 1, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
		zend_string_release_ex(ini_name, 0);
		if (result == FAILURE) {
			RETVAL_FALSE;
			goto cleanup;
		}
	}
	if (samesite) {
		ini_name = ZSTR_INIT_LITERAL("session.cookie_samesite", 0);
		result = zend_alter_ini_entry(ini_name, samesite, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
		zend_string_release_ex(ini_name, 0);
		if (result == FAILURE) {
			RETVAL_FALSE;
			goto cleanup;
		}
	}

	RETVAL_TRUE;

cleanup:
	if (lifetime) zend_string_release(lifetime);
	if (found > 0) {
		if (path) zend_string_release(path);
		if (domain) zend_string_release(domain);
		if (samesite) zend_string_release(samesite);
	}
}

PHP_FUNCTION(session_get_cookie_params)
{
	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	array_init(return_value);

	add_assoc_long(return_value, "lifetime", PS(cookie_lifetime));
	add_assoc_str(return_value, "path", zend_string_dup(PS(cookie_path), false));
	add_assoc_str(return_value, "domain", zend_string_dup(PS(cookie_domain), false));
	add_assoc_bool(return_value, "secure", PS(cookie_secure));
	add_assoc_bool(return_value, "httponly", PS(cookie_httponly));
	add_assoc_str(return_value, "samesite", zend_string_dup(PS(cookie_samesite), false));
}

/* Return the current session name. If new name is given, the session name is replaced with new name */
PHP_FUNCTION(session_name)
{
	zend_string *name = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|P!", &name) == FAILURE) {
		RETURN_THROWS();
	}

	if (name && PS(session_status) == php_session_active) {
		php_session_session_already_started_error(E_WARNING, "Session name cannot be changed when a session is active");
		RETURN_FALSE;
	}

	if (name && SG(headers_sent)) {
		php_session_headers_already_sent_error(E_WARNING, "Session name cannot be changed after headers have already been sent");
		RETURN_FALSE;
	}

	RETVAL_STRINGL(ZSTR_VAL(PS(session_name)), ZSTR_LEN(PS(session_name)));

	if (name) {
		zend_string *ini_name = ZSTR_INIT_LITERAL("session.name", 0);
		zend_alter_ini_entry(ini_name, name, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
		zend_string_release_ex(ini_name, 0);
	}
}

/* Return the current module name used for accessing session data. If newname is given, the module name is replaced with newname */
PHP_FUNCTION(session_module_name)
{
	zend_string *name = NULL;
	zend_string *ini_name;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|S!", &name) == FAILURE) {
		RETURN_THROWS();
	}

	if (name && PS(session_status) == php_session_active) {
		php_session_session_already_started_error(E_WARNING, "Session save handler module cannot be changed when a session is active");
		RETURN_FALSE;
	}

	if (name && SG(headers_sent)) {
		php_session_headers_already_sent_error(E_WARNING, "Session save handler module cannot be changed after headers have already been sent");
		RETURN_FALSE;
	}

	/* Set return_value to current module name */
	if (PS(mod) && PS(mod)->s_name) {
		RETVAL_STRING(PS(mod)->s_name);
	} else {
		RETVAL_EMPTY_STRING();
	}

	if (name) {
		if (zend_string_equals_ci(name, ZSTR_KNOWN(ZEND_STR_USER))) {
			zend_argument_value_error(1, "cannot be \"user\"");
			RETURN_THROWS();
		}
		if (!_php_find_ps_module(ZSTR_VAL(name))) {
			php_error_docref(NULL, E_WARNING, "Session handler module \"%s\" cannot be found", ZSTR_VAL(name));

			zval_ptr_dtor_str(return_value);
			RETURN_FALSE;
		}
		if (PS(mod_data) || PS(mod_user_implemented)) {
			PS(mod)->s_close(&PS(mod_data));
		}
		PS(mod_data) = NULL;

		ini_name = ZSTR_INIT_LITERAL("session.save_handler", 0);
		zend_alter_ini_entry(ini_name, name, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
		zend_string_release_ex(ini_name, 0);
	}
}

static bool can_session_handler_be_changed(void) {
	if (PS(session_status) == php_session_active) {
		php_session_session_already_started_error(E_WARNING, "Session save handler cannot be changed when a session is active");
		return false;
	}

	if (SG(headers_sent)) {
		php_session_headers_already_sent_error(E_WARNING, "Session save handler cannot be changed after headers have already been sent");
		return false;
	}

	return true;
}

static inline void set_user_save_handler_ini(void) {
	zend_string *ini_name, *ini_val;

	ini_name = ZSTR_INIT_LITERAL("session.save_handler", 0);
	ini_val = ZSTR_KNOWN(ZEND_STR_USER);
	PS(set_handler) = 1;
	zend_alter_ini_entry(ini_name, ini_val, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
	PS(set_handler) = 0;
	zend_string_release_ex(ini_val, 0);
	zend_string_release_ex(ini_name, 0);
}

#define SESSION_RELEASE_USER_HANDLER_OO(struct_name) \
	if (!Z_ISUNDEF(PS(mod_user_names).struct_name)) { \
		zval_ptr_dtor(&PS(mod_user_names).struct_name); \
		ZVAL_UNDEF(&PS(mod_user_names).struct_name); \
	}

#define SESSION_SET_USER_HANDLER_OO(struct_name, zstr_method_name) \
	array_init_size(&PS(mod_user_names).struct_name, 2); \
	Z_ADDREF_P(obj); \
	add_next_index_zval(&PS(mod_user_names).struct_name, obj); \
	add_next_index_str(&PS(mod_user_names).struct_name, zstr_method_name);

#define SESSION_SET_USER_HANDLER_OO_MANDATORY(struct_name, method_name) \
	if (!Z_ISUNDEF(PS(mod_user_names).struct_name)) { \
		zval_ptr_dtor(&PS(mod_user_names).struct_name); \
	} \
	array_init_size(&PS(mod_user_names).struct_name, 2); \
	Z_ADDREF_P(obj); \
	add_next_index_zval(&PS(mod_user_names).struct_name, obj); \
	add_next_index_str(&PS(mod_user_names).struct_name, zend_string_init(method_name, strlen(method_name), false));

#define SESSION_SET_USER_HANDLER_PROCEDURAL(struct_name, fci) \
	if (!Z_ISUNDEF(PS(mod_user_names).struct_name)) { \
		zval_ptr_dtor(&PS(mod_user_names).struct_name); \
	} \
	ZVAL_COPY(&PS(mod_user_names).struct_name, &fci.function_name);

#define SESSION_SET_USER_HANDLER_PROCEDURAL_OPTIONAL(struct_name, fci) \
	if (ZEND_FCI_INITIALIZED(fci)) { \
		SESSION_SET_USER_HANDLER_PROCEDURAL(struct_name, fci); \
	}

PHP_FUNCTION(session_set_save_handler)
{
	/* OOP Version */
	if (ZEND_NUM_ARGS() <= 2) {
		zval *obj = NULL;
		bool register_shutdown = 1;

		if (zend_parse_parameters(ZEND_NUM_ARGS(), "O|b", &obj, php_session_iface_entry, &register_shutdown) == FAILURE) {
			RETURN_THROWS();
		}

		if (!can_session_handler_be_changed()) {
			RETURN_FALSE;
		}

		if (PS(mod_user_class_name)) {
			zend_string_release(PS(mod_user_class_name));
		}
		PS(mod_user_class_name) = zend_string_copy(Z_OBJCE_P(obj)->name);

		/* Define mandatory handlers */
		SESSION_SET_USER_HANDLER_OO_MANDATORY(ps_open, "open");
		SESSION_SET_USER_HANDLER_OO_MANDATORY(ps_close, "close");
		SESSION_SET_USER_HANDLER_OO_MANDATORY(ps_read, "read");
		SESSION_SET_USER_HANDLER_OO_MANDATORY(ps_write, "write");
		SESSION_SET_USER_HANDLER_OO_MANDATORY(ps_destroy, "destroy");
		SESSION_SET_USER_HANDLER_OO_MANDATORY(ps_gc, "gc");

		/* Elements of object_methods HashTable are zend_function *method */
		HashTable *object_methods = &Z_OBJCE_P(obj)->function_table;

		/* Find implemented methods - SessionIdInterface (optional) */
		/* First release old handlers */
		SESSION_RELEASE_USER_HANDLER_OO(ps_create_sid);
		zend_string *create_sid_name = ZSTR_INIT_LITERAL("create_sid", false);
		if (instanceof_function(Z_OBJCE_P(obj), php_session_id_iface_entry)) {
			SESSION_SET_USER_HANDLER_OO(ps_create_sid, zend_string_copy(create_sid_name));
		} else if (zend_hash_find_ptr(object_methods, create_sid_name)) {
			/* For BC reasons we accept methods even if the class does not implement the interface */
			SESSION_SET_USER_HANDLER_OO(ps_create_sid, zend_string_copy(create_sid_name));
		}
		zend_string_release_ex(create_sid_name, false);

		/* Find implemented methods - SessionUpdateTimestampInterface (optional) */
		/* First release old handlers */
		SESSION_RELEASE_USER_HANDLER_OO(ps_validate_sid);
		SESSION_RELEASE_USER_HANDLER_OO(ps_update_timestamp);
		/* Method names need to be lowercase */
		zend_string *validate_sid_name = ZSTR_INIT_LITERAL("validateid", false);
		zend_string *update_timestamp_name = ZSTR_INIT_LITERAL("updatetimestamp", false);
		if (instanceof_function(Z_OBJCE_P(obj), php_session_update_timestamp_iface_entry)) {
			/* Validate ID handler */
			SESSION_SET_USER_HANDLER_OO(ps_validate_sid, zend_string_copy(validate_sid_name));
			/* Update Timestamp handler */
			SESSION_SET_USER_HANDLER_OO(ps_update_timestamp, zend_string_copy(update_timestamp_name));
		} else {
			/* For BC reasons we accept methods even if the class does not implement the interface */
			if (zend_hash_find_ptr(object_methods, validate_sid_name)) {
				/* For BC reasons we accept methods even if the class does not implement the interface */
				SESSION_SET_USER_HANDLER_OO(ps_validate_sid, zend_string_copy(validate_sid_name));
			}
			if (zend_hash_find_ptr(object_methods, update_timestamp_name)) {
				/* For BC reasons we accept methods even if the class does not implement the interface */
				SESSION_SET_USER_HANDLER_OO(ps_update_timestamp, zend_string_copy(update_timestamp_name));
			}
		}
		zend_string_release_ex(validate_sid_name, false);
		zend_string_release_ex(update_timestamp_name, false);

		if (register_shutdown) {
			/* create shutdown function */
			php_shutdown_function_entry shutdown_function_entry = {
				.fci_cache = empty_fcall_info_cache,
				.params = NULL,
				.param_count = 0,
			};
			zend_function *fn_entry = zend_hash_str_find_ptr(CG(function_table), ZEND_STRL("session_register_shutdown"));
			ZEND_ASSERT(fn_entry != NULL);
			shutdown_function_entry.fci_cache.function_handler = fn_entry;

			/* add shutdown function, removing the old one if it exists */
			if (!register_user_shutdown_function(ZEND_STRL("session_shutdown"), &shutdown_function_entry)) {
				php_error_docref(NULL, E_WARNING, "Unable to register session shutdown function");
				RETURN_FALSE;
			}
		} else {
			/* remove shutdown function */
			remove_user_shutdown_function("session_shutdown", strlen("session_shutdown"));
		}

		if (PS(session_status) != php_session_active && (!PS(mod) || PS(mod) != &ps_mod_user)) {
			set_user_save_handler_ini();
		}

		RETURN_TRUE;
	}

	php_error_docref(NULL, E_DEPRECATED, "Providing individual callbacks instead of an object implementing SessionHandlerInterface is deprecated");
	if (UNEXPECTED(EG(exception))) {
		RETURN_THROWS();
	}

	/* Procedural version */
	zend_fcall_info open_fci = {0};
	zend_fcall_info_cache open_fcc;
	zend_fcall_info close_fci = {0};
	zend_fcall_info_cache close_fcc;
	zend_fcall_info read_fci = {0};
	zend_fcall_info_cache read_fcc;
	zend_fcall_info write_fci = {0};
	zend_fcall_info_cache write_fcc;
	zend_fcall_info destroy_fci = {0};
	zend_fcall_info_cache destroy_fcc;
	zend_fcall_info gc_fci = {0};
	zend_fcall_info_cache gc_fcc;
	zend_fcall_info create_id_fci = {0};
	zend_fcall_info_cache create_id_fcc;
	zend_fcall_info validate_id_fci = {0};
	zend_fcall_info_cache validate_id_fcc;
	zend_fcall_info update_timestamp_fci = {0};
	zend_fcall_info_cache update_timestamp_fcc;

	if (zend_parse_parameters(ZEND_NUM_ARGS(),
		"ffffff|f!f!f!",
		&open_fci, &open_fcc,
		&close_fci, &close_fcc,
		&read_fci, &read_fcc,
		&write_fci, &write_fcc,
		&destroy_fci, &destroy_fcc,
		&gc_fci, &gc_fcc,
		&create_id_fci, &create_id_fcc,
		&validate_id_fci, &validate_id_fcc,
		&update_timestamp_fci, &update_timestamp_fcc) == FAILURE
	) {
		RETURN_THROWS();
	}
	if (!can_session_handler_be_changed()) {
		RETURN_FALSE;
	}

	/* If a custom session handler is already set, release relevant info */
	if (PS(mod_user_class_name)) {
		zend_string_release(PS(mod_user_class_name));
		PS(mod_user_class_name) = NULL;
	}

	/* remove shutdown function */
	remove_user_shutdown_function("session_shutdown", strlen("session_shutdown"));

	if (!PS(mod) || PS(mod) != &ps_mod_user) {
		set_user_save_handler_ini();
	}

	/* Define mandatory handlers */
	SESSION_SET_USER_HANDLER_PROCEDURAL(ps_open, open_fci);
	SESSION_SET_USER_HANDLER_PROCEDURAL(ps_close, close_fci);
	SESSION_SET_USER_HANDLER_PROCEDURAL(ps_read, read_fci);
	SESSION_SET_USER_HANDLER_PROCEDURAL(ps_write, write_fci);
	SESSION_SET_USER_HANDLER_PROCEDURAL(ps_destroy, destroy_fci);
	SESSION_SET_USER_HANDLER_PROCEDURAL(ps_gc, gc_fci);

	/* Check for optional handlers */
	SESSION_SET_USER_HANDLER_PROCEDURAL_OPTIONAL(ps_create_sid, create_id_fci);
	SESSION_SET_USER_HANDLER_PROCEDURAL_OPTIONAL(ps_validate_sid, validate_id_fci);
	SESSION_SET_USER_HANDLER_PROCEDURAL_OPTIONAL(ps_update_timestamp, update_timestamp_fci);

	RETURN_TRUE;
}

/* Return the current save path passed to module_name. If newname is given, the save path is replaced with newname */
PHP_FUNCTION(session_save_path)
{
	zend_string *name = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|P!", &name) == FAILURE) {
		RETURN_THROWS();
	}

	if (name && PS(session_status) == php_session_active) {
		php_session_session_already_started_error(E_WARNING, "Session save path cannot be changed when a session is active");
		RETURN_FALSE;
	}

	if (name && SG(headers_sent)) {
		php_session_headers_already_sent_error(E_WARNING, "Session save path cannot be changed after headers have already been sent");
		RETURN_FALSE;
	}

	RETVAL_STRINGL(ZSTR_VAL(PS(save_path)), ZSTR_LEN(PS(save_path)));

	if (name) {
		zend_string *ini_name = ZSTR_INIT_LITERAL("session.save_path", false);
		zend_alter_ini_entry(ini_name, name, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
		zend_string_release_ex(ini_name, false);
	}
}

/* Return the current session id. If newid is given, the session id is replaced with newid */
PHP_FUNCTION(session_id)
{
	zend_string *name = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|S!", &name) == FAILURE) {
		RETURN_THROWS();
	}

	if (name && PS(session_status) == php_session_active) {
		php_session_session_already_started_error(E_WARNING, "Session ID cannot be changed when a session is active");
		RETURN_FALSE;
	}

	if (name && PS(use_cookies) && SG(headers_sent)) {
		php_session_headers_already_sent_error(E_WARNING, "Session ID cannot be changed after headers have already been sent");
		RETURN_FALSE;
	}

	if (PS(id)) {
		/* keep compatibility for "\0" characters ???
		 * see: ext/session/tests/session_id_error3.phpt */
		size_t len = strlen(ZSTR_VAL(PS(id)));
		if (UNEXPECTED(len != ZSTR_LEN(PS(id)))) {
			RETVAL_NEW_STR(zend_string_init(ZSTR_VAL(PS(id)), len, 0));
		} else {
			RETVAL_STR_COPY(PS(id));
		}
	} else {
		RETVAL_EMPTY_STRING();
	}

	if (name) {
		if (PS(id)) {
			zend_string_release_ex(PS(id), 0);
		}
		PS(id) = zend_string_copy(name);
	}
}

/* Update the current session id with a newly generated one. If delete_old_session is set to true, remove the old session. */
PHP_FUNCTION(session_regenerate_id)
{
	bool del_ses = 0;
	zend_string *data;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|b", &del_ses) == FAILURE) {
		RETURN_THROWS();
	}

	if (PS(session_status) != php_session_active) {
		php_session_session_already_started_error(E_WARNING, "Session ID cannot be regenerated when there is no active session");
		RETURN_FALSE;
	}

	if (SG(headers_sent)) {
		php_session_headers_already_sent_error(E_WARNING, "Session ID cannot be regenerated after headers have already been sent");
		RETURN_FALSE;
	}

	/* Process old session data */
	if (del_ses) {
		if (PS(mod)->s_destroy(&PS(mod_data), PS(id)) == FAILURE) {
			PS(mod)->s_close(&PS(mod_data));
			PS(session_status) = php_session_none;
			if (!EG(exception)) {
				php_error_docref(NULL, E_WARNING, "Session object destruction failed. ID: %s (path: %s)", PS(mod)->s_name, ZSTR_VAL(PS(save_path)));
			}
			RETURN_FALSE;
		}
	} else {
		zend_result ret;
		data = php_session_encode();
		if (data) {
			ret = PS(mod)->s_write(&PS(mod_data), PS(id), data, PS(gc_maxlifetime));
			zend_string_release_ex(data, 0);
		} else {
			ret = PS(mod)->s_write(&PS(mod_data), PS(id), ZSTR_EMPTY_ALLOC(), PS(gc_maxlifetime));
		}
		if (ret == FAILURE) {
			PS(mod)->s_close(&PS(mod_data));
			PS(session_status) = php_session_none;
			php_error_docref(NULL, E_WARNING, "Session write failed. ID: %s (path: %s)", PS(mod)->s_name, ZSTR_VAL(PS(save_path)));
			RETURN_FALSE;
		}
	}
	PS(mod)->s_close(&PS(mod_data));

	/* New session data */
	if (PS(session_vars)) {
		zend_string_release_ex(PS(session_vars), 0);
		PS(session_vars) = NULL;
	}
	zend_string_release_ex(PS(id), 0);
	PS(id) = NULL;

	if (PS(mod)->s_open(&PS(mod_data), ZSTR_VAL(PS(save_path)), ZSTR_VAL(PS(session_name))) == FAILURE) {
		PS(session_status) = php_session_none;
		if (!EG(exception)) {
			zend_throw_error(NULL, "Failed to open session: %s (path: %s)", PS(mod)->s_name, ZSTR_VAL(PS(save_path)));
		}
		RETURN_THROWS();
	}

	PS(id) = PS(mod)->s_create_sid(&PS(mod_data));
	if (!PS(id)) {
		PS(session_status) = php_session_none;
		if (!EG(exception)) {
			zend_throw_error(NULL, "Failed to create new session ID: %s (path: %s)", PS(mod)->s_name, ZSTR_VAL(PS(save_path)));
		}
		RETURN_THROWS();
	}
	if (PS(use_strict_mode)) {
		if ((!PS(mod_user_implemented) && PS(mod)->s_validate_sid) || !Z_ISUNDEF(PS(mod_user_names).ps_validate_sid)) {
			int limit = 3;
			/* Try to generate non-existing ID */
			while (limit-- && PS(mod)->s_validate_sid(&PS(mod_data), PS(id)) == SUCCESS) {
				zend_string_release_ex(PS(id), 0);
				PS(id) = PS(mod)->s_create_sid(&PS(mod_data));
				if (!PS(id)) {
					PS(mod)->s_close(&PS(mod_data));
					PS(session_status) = php_session_none;
					if (!EG(exception)) {
						zend_throw_error(NULL, "Failed to create session ID by collision: %s (path: %s)", PS(mod)->s_name, ZSTR_VAL(PS(save_path)));
					}
					RETURN_THROWS();
				}
			}
		}
		// TODO warn that ID cannot be verified? else { }
	}
	/* Read is required to make new session data at this point. */
	if (PS(mod)->s_read(&PS(mod_data), PS(id), &data, PS(gc_maxlifetime)) == FAILURE) {
		PS(mod)->s_close(&PS(mod_data));
		PS(session_status) = php_session_none;
		if (!EG(exception)) {
			zend_throw_error(NULL, "Failed to create(read) session ID: %s (path: %s)", PS(mod)->s_name, ZSTR_VAL(PS(save_path)));
		}
		RETURN_THROWS();
	}
	if (data) {
		zend_string_release_ex(data, 0);
	}

	if (PS(use_cookies)) {
		PS(send_cookie) = 1;
	}
	if (php_session_reset_id() == FAILURE) {
		RETURN_FALSE;
	}

	RETURN_TRUE;
}

/* Generate new session ID. Intended for user save handlers. */
PHP_FUNCTION(session_create_id)
{
	zend_string *prefix = NULL, *new_id;
	smart_str id = {0};

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|P", &prefix) == FAILURE) {
		RETURN_THROWS();
	}

	if (prefix && ZSTR_LEN(prefix)) {
        if (ZSTR_LEN(prefix) > PS_MAX_SID_LENGTH) {
            zend_argument_value_error(1, "cannot be longer than %d characters", PS_MAX_SID_LENGTH);
            RETURN_THROWS();
        }
		if (php_session_valid_key(ZSTR_VAL(prefix)) == FAILURE) {
			/* E_ERROR raised for security reason. */
			php_error_docref(NULL, E_WARNING, "Prefix cannot contain special characters. Only the A-Z, a-z, 0-9, \"-\", and \",\" characters are allowed");
			RETURN_FALSE;
		} else {
			smart_str_append(&id, prefix);
		}
	}

	if (!PS(in_save_handler) && PS(session_status) == php_session_active) {
		int limit = 3;
		while (limit--) {
			new_id = PS(mod)->s_create_sid(&PS(mod_data));
			if (!PS(mod)->s_validate_sid || (PS(mod_user_implemented) && Z_ISUNDEF(PS(mod_user_names).ps_validate_sid))) {
				break;
			} else {
				/* Detect collision and retry */
				if (PS(mod)->s_validate_sid(&PS(mod_data), new_id) == SUCCESS) {
					zend_string_release_ex(new_id, 0);
					new_id = NULL;
					continue;
				}
				break;
			}
		}
	} else {
		new_id = php_session_create_id(NULL);
	}

	if (new_id) {
		smart_str_append(&id, new_id);
		zend_string_release_ex(new_id, 0);
	} else {
		smart_str_free(&id);
		php_error_docref(NULL, E_WARNING, "Failed to create new ID");
		RETURN_FALSE;
	}
	RETVAL_STR(smart_str_extract(&id));
}

/* Return the current cache limiter. If new_cache_limited is given, the current cache_limiter is replaced with new_cache_limiter */
PHP_FUNCTION(session_cache_limiter)
{
	zend_string *limiter = NULL;
	zend_string *ini_name;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|S!", &limiter) == FAILURE) {
		RETURN_THROWS();
	}

	if (limiter && PS(session_status) == php_session_active) {
		php_session_session_already_started_error(E_WARNING, "Session cache limiter cannot be changed when a session is active");
		RETURN_FALSE;
	}

	if (limiter && SG(headers_sent)) {
		php_session_headers_already_sent_error(E_WARNING, "Session cache limiter cannot be changed after headers have already been sent");
		RETURN_FALSE;
	}

	RETVAL_STRINGL(ZSTR_VAL(PS(cache_limiter)), ZSTR_LEN(PS(cache_limiter)));

	if (limiter) {
		ini_name = ZSTR_INIT_LITERAL("session.cache_limiter", 0);
		zend_alter_ini_entry(ini_name, limiter, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
		zend_string_release_ex(ini_name, 0);
	}
}

/* Return the current cache expire. If new_cache_expire is given, the current cache_expire is replaced with new_cache_expire */
PHP_FUNCTION(session_cache_expire)
{
	zend_long expires;
	bool expires_is_null = 1;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|l!", &expires, &expires_is_null) == FAILURE) {
		RETURN_THROWS();
	}

	if (!expires_is_null && PS(session_status) == php_session_active) {
		php_session_session_already_started_error(E_WARNING, "Session cache expiration cannot be changed when a session is active");
		RETURN_LONG(PS(cache_expire));
	}

	if (!expires_is_null && SG(headers_sent)) {
		php_session_headers_already_sent_error(E_WARNING, "Session cache expiration cannot be changed after headers have already been sent");
		RETURN_FALSE;
	}

	RETVAL_LONG(PS(cache_expire));

	if (!expires_is_null) {
		zend_string *ini_name = ZSTR_INIT_LITERAL("session.cache_expire", 0);
		zend_string *ini_value = zend_long_to_str(expires);
		zend_alter_ini_entry(ini_name, ini_value, ZEND_INI_USER, ZEND_INI_STAGE_RUNTIME);
		zend_string_release_ex(ini_name, 0);
		zend_string_release_ex(ini_value, 0);
	}
}

/* Serializes the current setup and returns the serialized representation */
PHP_FUNCTION(session_encode)
{
	zend_string *enc;

	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	enc = php_session_encode();
	if (enc == NULL) {
		RETURN_FALSE;
	}

	RETURN_STR(enc);
}

/* Deserializes data and reinitializes the variables */
PHP_FUNCTION(session_decode)
{
	zend_string *str = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "S", &str) == FAILURE) {
		RETURN_THROWS();
	}

	if (PS(session_status) != php_session_active) {
		php_error_docref(NULL, E_WARNING, "Session data cannot be decoded when there is no active session");
		RETURN_FALSE;
	}

	if (php_session_decode(str) == FAILURE) {
		RETURN_FALSE;
	}
	RETURN_TRUE;
}

static zend_result php_session_start_set_ini(zend_string *varname, zend_string *new_value) {
	zend_result ret;
	smart_str buf ={0};
	smart_str_appends(&buf, "session");
	smart_str_appendc(&buf, '.');
	smart_str_append(&buf, varname);
	smart_str_0(&buf);
	ret = zend_alter_ini_entry_ex(buf.s, new_value, PHP_INI_USER, PHP_INI_STAGE_RUNTIME, 0);
	smart_str_free(&buf);
	return ret;
}

PHP_FUNCTION(session_start)
{
	zval *options = NULL;
	zval *value;
	zend_string *str_idx;
	bool read_and_close = false;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|a", &options) == FAILURE) {
		RETURN_THROWS();
	}

	if (PS(session_status) == php_session_active) {
		php_session_session_already_started_error(E_NOTICE, "Ignoring session_start() because a session is already active");
		RETURN_TRUE;
	}

	/*
	 * TODO: To prevent unusable session with trans sid, actual output started status is
	 * required. i.e. There shouldn't be any outputs in output buffer, otherwise session
	 * module is unable to rewrite output.
	 */
	if (PS(use_cookies) && SG(headers_sent)) {
		php_session_headers_already_sent_error(E_WARNING, "Session cannot be started after headers have already been sent");
		RETURN_FALSE;
	}

	/* set options */
	if (options) {
		ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(options), str_idx, value) {
			if (UNEXPECTED(!str_idx)) {
				zend_argument_value_error(1, "must be of type array with keys as string");
				RETURN_THROWS();
			}
			switch(Z_TYPE_P(value)) {
				case IS_STRING:
				case IS_TRUE:
				case IS_FALSE:
				case IS_LONG:
					if (zend_string_equals_literal(str_idx, "read_and_close")) {
						zend_long tmp;
						if (Z_TYPE_P(value) != IS_STRING) {
							tmp = zval_get_long(value);
						} else {
							if (is_numeric_str_function(Z_STR_P(value), &tmp, NULL) != IS_LONG) {
								zend_type_error("%s(): Option \"%s\" value must be of type compatible with int, \"%s\" given",
										get_active_function_name(), ZSTR_VAL(str_idx), Z_STRVAL_P(value)
									       );
								RETURN_THROWS();
							}
						}
						read_and_close = (tmp > 0);
					} else {
						zend_string *tmp_val;
						zend_string *val = zval_get_tmp_string(value, &tmp_val);
						if (php_session_start_set_ini(str_idx, val) == FAILURE) {
							php_error_docref(NULL, E_WARNING, "Setting option \"%s\" failed", ZSTR_VAL(str_idx));
						}
						zend_tmp_string_release(tmp_val);
					}
					break;
				default:
					zend_type_error("%s(): Option \"%s\" must be of type string|int|bool, %s given",
							get_active_function_name(), ZSTR_VAL(str_idx), zend_zval_value_name(value)
						       );
					RETURN_THROWS();
			}
		} ZEND_HASH_FOREACH_END();
	}

	php_session_start();

	if (PS(session_status) != php_session_active) {
		IF_SESSION_VARS() {
			zval *sess_var = Z_REFVAL(PS(http_session_vars));
			SEPARATE_ARRAY(sess_var);
			/* Clean $_SESSION. */
			zend_hash_clean(Z_ARRVAL_P(sess_var));
		}
		RETURN_FALSE;
	}

	if (read_and_close) {
		php_session_flush(0);
	}

	RETURN_TRUE;
}

PHP_FUNCTION(session_destroy)
{
	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	RETURN_BOOL(php_session_destroy() == SUCCESS);
}

PHP_FUNCTION(session_unset)
{
	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	if (PS(session_status) != php_session_active) {
		RETURN_FALSE;
	}

	IF_SESSION_VARS() {
		zval *sess_var = Z_REFVAL(PS(http_session_vars));
		SEPARATE_ARRAY(sess_var);

		/* Clean $_SESSION. */
		zend_hash_clean(Z_ARRVAL_P(sess_var));
	}
	RETURN_TRUE;
}

PHP_FUNCTION(session_gc)
{
	zend_long num;

	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	if (PS(session_status) != php_session_active) {
		php_error_docref(NULL, E_WARNING, "Session cannot be garbage collected when there is no active session");
		RETURN_FALSE;
	}

	num = php_session_gc(1);
	if (num < 0) {
		RETURN_FALSE;
	}

	RETURN_LONG(num);
}


PHP_FUNCTION(session_write_close)
{
	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	if (PS(session_status) != php_session_active) {
		RETURN_FALSE;
	}
	php_session_flush(1);
	RETURN_TRUE;
}

/* Abort session and end session. Session data will not be written */
PHP_FUNCTION(session_abort)
{
	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	if (PS(session_status) != php_session_active) {
		RETURN_FALSE;
	}
	php_session_abort();
	RETURN_TRUE;
}

/* Reset session data from saved session data */
PHP_FUNCTION(session_reset)
{
	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	if (PS(session_status) != php_session_active) {
		RETURN_FALSE;
	}
	php_session_reset();
	RETURN_TRUE;
}

PHP_FUNCTION(session_status)
{
	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	RETURN_LONG(PS(session_status));
}

/* Registers session_write_close() as a shutdown function */
PHP_FUNCTION(session_register_shutdown)
{
	php_shutdown_function_entry shutdown_function_entry = {
		.fci_cache = empty_fcall_info_cache,
		.params = NULL,
		.param_count = 0,
	};

	ZEND_PARSE_PARAMETERS_NONE();

	/* This function is registered itself as a shutdown function by
	 * session_set_save_handler($obj). The reason we now register another
	 * shutdown function is in case the user registered their own shutdown
	 * function after calling session_set_save_handler(), which expects
	 * the session still to be available.
	 */
	zend_function *fn_entry = zend_hash_str_find_ptr(CG(function_table), ZEND_STRL("session_write_close"));
	ZEND_ASSERT(fn_entry != NULL);
	shutdown_function_entry.fci_cache.function_handler = fn_entry;

	if (!append_user_shutdown_function(&shutdown_function_entry)) {
		/* Unable to register shutdown function, presumably because of lack
		 * of memory, so flush the session now. It would be done in rshutdown
		 * anyway but the handler will have had it's dtor called by then.
		 * If the user does have a later shutdown function which needs the
		 * session then tough luck.
		 */
		php_session_flush(1);
		php_error_docref(NULL, E_WARNING, "Session shutdown function cannot be registered");
	}
}

/* ********************************
   * Module Setup and Destruction *
   ******************************** */

static zend_result php_rinit_session(bool auto_start)
{
	php_rinit_session_globals();

	PS(mod) = NULL;
	{
		char *value;

		value = zend_ini_string(ZEND_STRL("session.save_handler"), false);
		if (value) {
			PS(mod) = _php_find_ps_module(value);
		}
	}

	if (PS(serializer) == NULL) {
		char *value;

		value = zend_ini_string(ZEND_STRL("session.serialize_handler"), false);
		if (value) {
			PS(serializer) = _php_find_ps_serializer(value);
		}
	}

	if (PS(mod) == NULL || PS(serializer) == NULL) {
		/* current status is unusable */
		PS(session_status) = php_session_disabled;
		return SUCCESS;
	}

	if (auto_start) {
		php_session_start();
	}

	return SUCCESS;
}

static PHP_RINIT_FUNCTION(session)
{
	return php_rinit_session(PS(auto_start));
}

#define SESSION_FREE_USER_HANDLER(struct_name) \
	if (!Z_ISUNDEF(PS(mod_user_names).struct_name)) { \
		zval_ptr_dtor(&PS(mod_user_names).struct_name); \
		ZVAL_UNDEF(&PS(mod_user_names).struct_name); \
	}


static PHP_RSHUTDOWN_FUNCTION(session)
{
	if (PS(session_status) == php_session_active) {
		zend_try {
			php_session_flush(1);
		} zend_end_try();
	}
	php_rshutdown_session_globals();

	/* this should NOT be done in php_rshutdown_session_globals() */
	/* Free user defined handlers */
	SESSION_FREE_USER_HANDLER(ps_open);
	SESSION_FREE_USER_HANDLER(ps_close);
	SESSION_FREE_USER_HANDLER(ps_read);
	SESSION_FREE_USER_HANDLER(ps_write);
	SESSION_FREE_USER_HANDLER(ps_destroy);
	SESSION_FREE_USER_HANDLER(ps_gc);
	SESSION_FREE_USER_HANDLER(ps_create_sid);
	SESSION_FREE_USER_HANDLER(ps_validate_sid);
	SESSION_FREE_USER_HANDLER(ps_update_timestamp);

	return SUCCESS;
}

static PHP_GINIT_FUNCTION(ps)
{
#if defined(COMPILE_DL_SESSION) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	ps_globals->save_path = NULL;
	ps_globals->session_name = NULL;
	ps_globals->id = NULL;
	ps_globals->mod = NULL;
	ps_globals->serializer = NULL;
	ps_globals->mod_data = NULL;
	ps_globals->session_status = php_session_none;
	ps_globals->default_mod = NULL;
	ps_globals->mod_user_implemented = 0;
	ps_globals->mod_user_class_name = NULL;
	ps_globals->mod_user_is_open = 0;
	ps_globals->session_vars = NULL;
	ps_globals->set_handler = 0;
	ps_globals->session_started_filename = NULL;
	ps_globals->session_started_lineno = 0;
	/* Unset user defined handlers */
	ZVAL_UNDEF(&ps_globals->mod_user_names.ps_open);
	ZVAL_UNDEF(&ps_globals->mod_user_names.ps_close);
	ZVAL_UNDEF(&ps_globals->mod_user_names.ps_read);
	ZVAL_UNDEF(&ps_globals->mod_user_names.ps_write);
	ZVAL_UNDEF(&ps_globals->mod_user_names.ps_destroy);
	ZVAL_UNDEF(&ps_globals->mod_user_names.ps_gc);
	ZVAL_UNDEF(&ps_globals->mod_user_names.ps_create_sid);
	ZVAL_UNDEF(&ps_globals->mod_user_names.ps_validate_sid);
	ZVAL_UNDEF(&ps_globals->mod_user_names.ps_update_timestamp);
	ZVAL_UNDEF(&ps_globals->http_session_vars);

	ps_globals->random = (php_random_algo_with_state){
		.algo = &php_random_algo_pcgoneseq128xslrr64,
		.state = &ps_globals->random_state,
	};
	php_random_uint128_t seed;
	if (php_random_bytes_silent(&seed, sizeof(seed)) == FAILURE) {
		seed = php_random_uint128_constant(
			php_random_generate_fallback_seed(),
			php_random_generate_fallback_seed()
		);
	}
	php_random_pcgoneseq128xslrr64_seed128(ps_globals->random.state, seed);
}

static PHP_MINIT_FUNCTION(session)
{
	zend_register_auto_global(zend_string_init_interned(ZEND_STRL("_SESSION"), true), false, NULL);

	my_module_number = module_number;
	PS(module_number) = module_number;

	PS(session_status) = php_session_none;
	REGISTER_INI_ENTRIES();

#ifdef HAVE_LIBMM
	PHP_MINIT(ps_mm) (INIT_FUNC_ARGS_PASSTHRU);
#endif
	php_session_rfc1867_orig_callback = php_rfc1867_callback;
	php_rfc1867_callback = php_session_rfc1867_callback;

	/* Register interfaces */
	php_session_iface_entry = register_class_SessionHandlerInterface();

	php_session_id_iface_entry = register_class_SessionIdInterface();

	php_session_update_timestamp_iface_entry = register_class_SessionUpdateTimestampHandlerInterface();

	/* Register base class */
	php_session_class_entry = register_class_SessionHandler(php_session_iface_entry, php_session_id_iface_entry);

	register_session_symbols(module_number);

	return SUCCESS;
}

static PHP_MSHUTDOWN_FUNCTION(session)
{
	UNREGISTER_INI_ENTRIES();

#ifdef HAVE_LIBMM
	PHP_MSHUTDOWN(ps_mm) (SHUTDOWN_FUNC_ARGS_PASSTHRU);
#endif

	/* reset rfc1867 callbacks */
	php_session_rfc1867_orig_callback = NULL;
	if (php_rfc1867_callback == php_session_rfc1867_callback) {
		php_rfc1867_callback = NULL;
	}

	ps_serializers[PREDEFINED_SERIALIZERS].name = NULL;
	memset(ZEND_VOIDP(&ps_modules[PREDEFINED_MODULES]), 0, (MAX_MODULES-PREDEFINED_MODULES)*sizeof(ps_module *));

	return SUCCESS;
}

static PHP_MINFO_FUNCTION(session)
{
	const ps_module **mod;
	ps_serializer *ser;
	smart_str save_handlers = {0};
	smart_str ser_handlers = {0};
	int i;

	/* Get save handlers */
	for (i = 0, mod = ps_modules; i < MAX_MODULES; i++, mod++) {
		if (*mod && (*mod)->s_name) {
			smart_str_appends(&save_handlers, (*mod)->s_name);
			smart_str_appendc(&save_handlers, ' ');
		}
	}

	/* Get serializer handlers */
	for (i = 0, ser = ps_serializers; i < MAX_SERIALIZERS; i++, ser++) {
		if (ser->name) {
			smart_str_appends(&ser_handlers, ser->name);
			smart_str_appendc(&ser_handlers, ' ');
		}
	}

	php_info_print_table_start();
	php_info_print_table_row(2, "Session Support", "enabled" );

	if (save_handlers.s) {
		smart_str_0(&save_handlers);
		php_info_print_table_row(2, "Registered save handlers", ZSTR_VAL(save_handlers.s));
		smart_str_free(&save_handlers);
	} else {
		php_info_print_table_row(2, "Registered save handlers", "none");
	}

	if (ser_handlers.s) {
		smart_str_0(&ser_handlers);
		php_info_print_table_row(2, "Registered serializer handlers", ZSTR_VAL(ser_handlers.s));
		smart_str_free(&ser_handlers);
	} else {
		php_info_print_table_row(2, "Registered serializer handlers", "none");
	}

	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}

static const zend_module_dep session_deps[] = {
	ZEND_MOD_OPTIONAL("spl")
	ZEND_MOD_END
};

/* ************************
   * Upload hook handling *
   ************************ */

static bool early_find_sid_in(zval *dest, int where, php_session_rfc1867_progress *progress)
{
	zval *ppid;

	if (Z_ISUNDEF(PG(http_globals)[where])) {
		return 0;
	}

	if ((ppid = zend_hash_find(Z_ARRVAL(PG(http_globals)[where]), PS(session_name)))
			&& Z_TYPE_P(ppid) == IS_STRING) {
		zval_ptr_dtor(dest);
		ZVAL_COPY_DEREF(dest, ppid);
		return 1;
	}

	return 0;
}

static void php_session_rfc1867_early_find_sid(php_session_rfc1867_progress *progress)
{

	if (PS(use_cookies)) {
		sapi_module.treat_data(PARSE_COOKIE, NULL, NULL);
		if (early_find_sid_in(&progress->sid, TRACK_VARS_COOKIE, progress)) {
			progress->apply_trans_sid = 0;
			return;
		}
	}
	if (PS(use_only_cookies)) {
		return;
	}
	sapi_module.treat_data(PARSE_GET, NULL, NULL);
	early_find_sid_in(&progress->sid, TRACK_VARS_GET, progress);
}

static bool php_check_cancel_upload(php_session_rfc1867_progress *progress)
{
	zval *progress_ary, *cancel_upload;

	if ((progress_ary = zend_symtable_find(Z_ARRVAL_P(Z_REFVAL(PS(http_session_vars))), progress->key.s)) == NULL) {
		return 0;
	}
	if (Z_TYPE_P(progress_ary) != IS_ARRAY) {
		return 0;
	}
	if ((cancel_upload = zend_hash_str_find(Z_ARRVAL_P(progress_ary), ZEND_STRL("cancel_upload"))) == NULL) {
		return 0;
	}
	return Z_TYPE_P(cancel_upload) == IS_TRUE;
}

static void php_session_rfc1867_update(php_session_rfc1867_progress *progress, bool force_update)
{
	if (!force_update) {
		if (Z_LVAL_P(progress->post_bytes_processed) < progress->next_update) {
			return;
		}
#ifdef HAVE_GETTIMEOFDAY
		if (PS(rfc1867_min_freq) > 0.0) {
			struct timeval tv = {0};
			double dtv;
			gettimeofday(&tv, NULL);
			dtv = (double) tv.tv_sec + tv.tv_usec / 1000000.0;
			if (dtv < progress->next_update_time) {
				return;
			}
			progress->next_update_time = dtv + PS(rfc1867_min_freq);
		}
#endif
		progress->next_update = Z_LVAL_P(progress->post_bytes_processed) + progress->update_step;
	}

	php_session_initialize();
	PS(session_status) = php_session_active;
	IF_SESSION_VARS() {
		zval *sess_var = Z_REFVAL(PS(http_session_vars));
		SEPARATE_ARRAY(sess_var);

		progress->cancel_upload |= php_check_cancel_upload(progress);
		Z_TRY_ADDREF(progress->data);
		zend_hash_update(Z_ARRVAL_P(sess_var), progress->key.s, &progress->data);
	}
	php_session_flush(1);
}

static void php_session_rfc1867_cleanup(php_session_rfc1867_progress *progress)
{
	php_session_initialize();
	PS(session_status) = php_session_active;
	IF_SESSION_VARS() {
		zval *sess_var = Z_REFVAL(PS(http_session_vars));
		SEPARATE_ARRAY(sess_var);
		zend_hash_del(Z_ARRVAL_P(sess_var), progress->key.s);
	}
	php_session_flush(1);
}

static zend_result php_session_rfc1867_callback(unsigned int event, void *event_data, void **extra)
{
	php_session_rfc1867_progress *progress;
	zend_result retval = SUCCESS;

	if (php_session_rfc1867_orig_callback) {
		retval = php_session_rfc1867_orig_callback(event, event_data, extra);
	}
	if (!PS(rfc1867_enabled)) {
		return retval;
	}

	progress = PS(rfc1867_progress);

	switch(event) {
		case MULTIPART_EVENT_START: {
			multipart_event_start *data = (multipart_event_start *) event_data;
			progress = ecalloc(1, sizeof(php_session_rfc1867_progress));
			progress->content_length = data->content_length;
			PS(rfc1867_progress) = progress;
		}
		break;
		case MULTIPART_EVENT_FORMDATA: {
			multipart_event_formdata *data = (multipart_event_formdata *) event_data;
			size_t value_len;

			if (Z_TYPE(progress->sid) && progress->key.s) {
				break;
			}

			/* orig callback may have modified *data->newlength */
			if (data->newlength) {
				value_len = *data->newlength;
			} else {
				value_len = data->length;
			}

			if (data->name && data->value && value_len) {
				size_t name_len = strlen(data->name);

				if (zend_string_equals_cstr(PS(session_name), data->name, name_len)) {
					zval_ptr_dtor(&progress->sid);
					ZVAL_STRINGL(&progress->sid, (*data->value), value_len);
				} else if (zend_string_equals_cstr(PS(rfc1867_name), data->name, name_len)) {
					smart_str_free(&progress->key);
					smart_str_append(&progress->key, PS(rfc1867_prefix));
					smart_str_appendl(&progress->key, *data->value, value_len);
					smart_str_0(&progress->key);

					progress->apply_trans_sid = APPLY_TRANS_SID;
					php_session_rfc1867_early_find_sid(progress);
				}
			}
		}
		break;
		case MULTIPART_EVENT_FILE_START: {
			multipart_event_file_start *data = (multipart_event_file_start *) event_data;

			/* Do nothing when $_POST["PHP_SESSION_UPLOAD_PROGRESS"] is not set
			 * or when we have no session id */
			if (!Z_TYPE(progress->sid) || !progress->key.s) {
				break;
			}

			/* First FILE_START event, initializing data */
			if (Z_ISUNDEF(progress->data)) {

				if (PS(rfc1867_freq) >= 0) {
					progress->update_step = PS(rfc1867_freq);
				} else if (PS(rfc1867_freq) < 0) { /* % of total size */
					progress->update_step = progress->content_length * -PS(rfc1867_freq) / 100;
				}
				progress->next_update = 0;
				progress->next_update_time = 0.0;

				array_init(&progress->data);
				array_init(&progress->files);

				add_assoc_long_ex(&progress->data, ZEND_STRL("start_time"), (zend_long)sapi_get_request_time());
				add_assoc_long_ex(&progress->data, ZEND_STRL("content_length"), progress->content_length);
				add_assoc_long_ex(&progress->data, ZEND_STRL("bytes_processed"), data->post_bytes_processed);
				add_assoc_bool_ex(&progress->data, ZEND_STRL("done"), false);
				add_assoc_zval_ex(&progress->data, ZEND_STRL("files"), &progress->files);

				progress->post_bytes_processed = zend_hash_str_find(Z_ARRVAL(progress->data), ZEND_STRL("bytes_processed"));

				php_rinit_session(0);
				PS(id) = zend_string_copy(Z_STR(progress->sid));
				if (progress->apply_trans_sid) {
					/* Enable trans sid by modifying flags */
					PS(use_trans_sid) = 1;
					PS(use_only_cookies) = 0;
				}
				PS(send_cookie) = 0;
			}

			array_init(&progress->current_file);

			/* Each uploaded file has its own array. Trying to make it close to $_FILES entries. */
			add_assoc_string_ex(&progress->current_file, ZEND_STRL("field_name"), data->name);
			add_assoc_string_ex(&progress->current_file, ZEND_STRL("name"), *data->filename);
			add_assoc_null_ex(&progress->current_file, ZEND_STRL("tmp_name"));
			add_assoc_long_ex(&progress->current_file, ZEND_STRL("error"), 0);

			add_assoc_bool_ex(&progress->current_file, ZEND_STRL("done"), 0);
			add_assoc_long_ex(&progress->current_file, ZEND_STRL("start_time"), (zend_long)time(NULL));
			add_assoc_long_ex(&progress->current_file, ZEND_STRL("bytes_processed"), 0);

			add_next_index_zval(&progress->files, &progress->current_file);

			progress->current_file_bytes_processed = zend_hash_str_find(Z_ARRVAL(progress->current_file), ZEND_STRL("bytes_processed"));

			Z_LVAL_P(progress->current_file_bytes_processed) =  data->post_bytes_processed;
			php_session_rfc1867_update(progress, 0);
		}
		break;
		case MULTIPART_EVENT_FILE_DATA: {
			multipart_event_file_data *data = (multipart_event_file_data *) event_data;

			if (!Z_TYPE(progress->sid) || !progress->key.s) {
				break;
			}

			Z_LVAL_P(progress->current_file_bytes_processed) = data->offset + data->length;
			Z_LVAL_P(progress->post_bytes_processed) = data->post_bytes_processed;

			php_session_rfc1867_update(progress, 0);
		}
		break;
		case MULTIPART_EVENT_FILE_END: {
			multipart_event_file_end *data = (multipart_event_file_end *) event_data;

			if (!Z_TYPE(progress->sid) || !progress->key.s) {
				break;
			}

			if (data->temp_filename) {
				add_assoc_string_ex(&progress->current_file, ZEND_STRL("tmp_name"), data->temp_filename);
			}

			add_assoc_long_ex(&progress->current_file, ZEND_STRL("error"), data->cancel_upload);
			add_assoc_bool_ex(&progress->current_file, ZEND_STRL("done"),  1);

			Z_LVAL_P(progress->post_bytes_processed) = data->post_bytes_processed;

			php_session_rfc1867_update(progress, 0);
		}
		break;
		case MULTIPART_EVENT_END: {
			multipart_event_end *data = (multipart_event_end *) event_data;

			if (Z_TYPE(progress->sid) && progress->key.s) {
				if (PS(rfc1867_cleanup)) {
					php_session_rfc1867_cleanup(progress);
				} else {
					if (!Z_ISUNDEF(progress->data)) {
						SEPARATE_ARRAY(&progress->data);
						add_assoc_bool_ex(&progress->data, ZEND_STRL("done"), 1);
						Z_LVAL_P(progress->post_bytes_processed) = data->post_bytes_processed;
						php_session_rfc1867_update(progress, 1);
					}
				}
				php_rshutdown_session_globals();
			}

			if (!Z_ISUNDEF(progress->data)) {
				zval_ptr_dtor(&progress->data);
			}
			zval_ptr_dtor(&progress->sid);
			smart_str_free(&progress->key);
			efree(progress);
			progress = NULL;
			PS(rfc1867_progress) = NULL;
		}
		break;
	}

	if (progress && progress->cancel_upload) {
		return FAILURE;
	}
	return retval;
}

zend_module_entry session_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	session_deps,
	"session",
	ext_functions,
	PHP_MINIT(session), PHP_MSHUTDOWN(session),
	PHP_RINIT(session), PHP_RSHUTDOWN(session),
	PHP_MINFO(session),
	PHP_SESSION_VERSION,
	PHP_MODULE_GLOBALS(ps),
	PHP_GINIT(ps),
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_SESSION
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(session)
#endif
