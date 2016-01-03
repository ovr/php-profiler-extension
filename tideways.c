/*
 *  Copyright (c) 2009 Facebook
 *  Copyright (c) 2014 Qafoo GmbH
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>

#if __APPLE__
#include <mach/mach_init.h>
#include <mach/mach_time.h>
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/standard/file.h"
#include "php_tideways.h"
#include "zend_extensions.h"
#include "zend_gc.h"

#include "ext/standard/url.h"
#include "ext/pdo/php_pdo_driver.h"
#include "zend_stream.h"

#ifdef PHP_TIDEWAYS_HAVE_CURL
#if PHP_VERSION_ID > 50399
#include <curl/curl.h>
#include <curl/easy.h>
#endif
#endif


/**
 * **********************
 * GLOBAL MACRO CONSTANTS
 * **********************
 */

/* Tideways version                           */
#define TIDEWAYS_VERSION       "3.0.3"

/* Fictitious function name to represent top of the call tree. The paranthesis
 * in the name is to ensure we don't conflict with user function names.  */
#define ROOT_SYMBOL                "main()"

/* Size of a temp scratch buffer            */
#define SCRATCH_BUF_LEN            512

/* Hierarchical profiling flags.
 *
 * Note: Function call counts and wall (elapsed) time are always profiled.
 * The following optional flags can be used to control other aspects of
 * profiling.
 */
#define TIDEWAYS_FLAGS_NO_BUILTINS   0x0001 /* do not profile builtins */
#define TIDEWAYS_FLAGS_CPU           0x0002 /* gather CPU times for funcs */
#define TIDEWAYS_FLAGS_MEMORY        0x0004 /* gather memory usage for funcs */
#define TIDEWAYS_FLAGS_NO_USERLAND   0x0008 /* do not profile userland functions */
#define TIDEWAYS_FLAGS_NO_COMPILE    0x0010 /* do not profile require/include/eval */
#define TIDEWAYS_FLAGS_NO_SPANS      0x0020
#define TIDEWAYS_FLAGS_NO_HIERACHICAL 0x0040

/* Constant for ignoring functions, transparent to hierarchical profile */
#define TIDEWAYS_MAX_FILTERED_FUNCTIONS  256
#define TIDEWAYS_FILTERED_FUNCTION_SIZE                           \
               ((TIDEWAYS_MAX_FILTERED_FUNCTIONS + 7)/8)
#define TIDEWAYS_MAX_ARGUMENT_LEN 256

#if !defined(uint64)
typedef unsigned long long uint64;
#endif
#if !defined(uint32)
typedef unsigned int uint32;
#endif
#if !defined(uint8)
typedef unsigned char uint8;
#endif

#define register_trace_callback(function_name, cb) zend_hash_update(hp_globals.trace_callbacks, function_name, sizeof(function_name), &cb, sizeof(tw_trace_callback*), NULL);
#define register_trace_callback_len(function_name, len, cb) zend_hash_update(hp_globals.trace_callbacks, function_name, len+1, &cb, sizeof(tw_trace_callback*), NULL);

/**
 * *****************************
 * GLOBAL DATATYPES AND TYPEDEFS
 * *****************************
 */

/* Tideways maintains a stack of entries being profiled. The memory for the entry
 * is passed by the layer that invokes BEGIN_PROFILING(), e.g. the hp_execute()
 * function. Often, this is just C-stack memory.
 *
 * This structure is a convenient place to track start time of a particular
 * profile operation, recursion depth, and the name of the function being
 * profiled. */
typedef struct hp_entry_t {
	char                   *name_hprof;                       /* function name */
	int                     rlvl_hprof;        /* recursion level for function */
	uint64                  tsc_start;         /* start value for wall clock timer */
	uint64					cpu_start;		   /* start value for CPU clock timer */
	long int                mu_start_hprof;                    /* memory usage */
	long int                pmu_start_hprof;              /* peak memory usage */
	struct hp_entry_t      *prev_hprof;    /* ptr to prev entry being profiled */
	uint8                   hash_code;     /* hash_code for the function name  */
	long int				span_id; /* span id of this entry if any, otherwise -1 */
} hp_entry_t;

typedef struct hp_string {
	char *value;
	size_t length;
} hp_string;

typedef struct hp_function_map {
	char **names;
	uint8 filter[TIDEWAYS_FILTERED_FUNCTION_SIZE];
} hp_function_map;

typedef struct tw_watch_callback {
	zend_fcall_info fci;
	zend_fcall_info_cache fcic;
} tw_watch_callback;

/* Tideways's global state.
 *
 * This structure is instantiated once.  Initialize defaults for attributes in
 * hp_init_profiler_state() Cleanup/free attributes in
 * hp_clean_profiler_state() */
typedef struct hp_global_t {

	/*       ----------   Global attributes:  -----------       */

	/* Indicates if Tideways is currently enabled */
	int              enabled;

	/* Indicates if Tideways was ever enabled during this request */
	int              ever_enabled;

	int				 prepend_overwritten;

	/* Holds all the Tideways statistics */
	zval            *stats_count;
	zval			*spans;
	long			current_span_id;
	uint64			start_time;

	zval			*backtrace;
	zval			*exception;

	/* Top of the profile stack */
	hp_entry_t      *entries;

	/* freelist of hp_entry_t chunks for reuse... */
	hp_entry_t      *entry_free_list;

	/* Function that determines the transaction name and callback */
	hp_string       *transaction_function;
	hp_string		*transaction_name;
	char			*root;

	hp_string		*exception_function;

	double timebase_factor;

	/* Tideways flags */
	uint32 tideways_flags;

	/* counter table indexed by hash value of function names. */
	uint8  func_hash_counters[256];

	/* Table of filtered function names and their filter */
	int     filtered_type; // 1 = blacklist, 2 = whitelist, 0 = nothing

	hp_function_map *filtered_functions;

	HashTable *trace_watch_callbacks;
	HashTable *trace_callbacks;
	HashTable *span_cache;

	zend_uint gc_runs; /* number of garbage collection runs */
	zend_uint gc_collected; /* number of collected items in garbage run */
	int compile_count;
	double compile_wt;
	uint64 cpu_start;
} hp_global_t;

#ifdef PHP_TIDEWAYS_HAVE_CURL
#if PHP_VERSION_ID > 50399
typedef struct hp_curl_t {
	struct {
		char str[CURL_ERROR_SIZE + 1];
		int  no;
	} err;

	void *free;

	struct {
		char *str;
		size_t str_len;
	} hdr;

	void ***thread_ctx;
	CURL *cp;
} hp_curl_t;
#endif
#endif

typedef long (*tw_trace_callback)(char *symbol, void **args, int args_len, zval *object TSRMLS_DC);

/**
 * ***********************
 * GLOBAL STATIC VARIABLES
 * ***********************
 */
/* Tideways global state */
static hp_global_t       hp_globals;

#if PHP_VERSION_ID < 50500
/* Pointer to the original execute function */
static ZEND_DLEXPORT void (*_zend_execute) (zend_op_array *ops TSRMLS_DC);

/* Pointer to the origianl execute_internal function */
static ZEND_DLEXPORT void (*_zend_execute_internal) (zend_execute_data *data,
                           int ret TSRMLS_DC);
#else
/* Pointer to the original execute function */
static void (*_zend_execute_ex) (zend_execute_data *execute_data TSRMLS_DC);

/* Pointer to the origianl execute_internal function */
static void (*_zend_execute_internal) (zend_execute_data *data,
                      struct _zend_fcall_info *fci, int ret TSRMLS_DC);
#endif

/* Pointer to the original compile function */
static zend_op_array * (*_zend_compile_file) (zend_file_handle *file_handle,
                                              int type TSRMLS_DC);

/* Pointer to the original compile string function (used by eval) */
static zend_op_array * (*_zend_compile_string) (zval *source_string, char *filename TSRMLS_DC);

/* error callback replacement functions */
void (*tideways_original_error_cb)(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args);
void tideways_error_cb(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args);

/* Bloom filter for function names to be ignored */
#define INDEX_2_BYTE(index)  (index >> 3)
#define INDEX_2_BIT(index)   (1 << (index & 0x7));


/**
 * ****************************
 * STATIC FUNCTION DECLARATIONS
 * ****************************
 */
static void hp_register_constants(INIT_FUNC_ARGS);

static void hp_begin(long tideways_flags TSRMLS_DC);
static void hp_stop(TSRMLS_D);
static void hp_end(TSRMLS_D);

static uint64 cycle_timer();

static void hp_free_the_free_list();
static hp_entry_t *hp_fast_alloc_hprof_entry();
static void hp_fast_free_hprof_entry(hp_entry_t *p);
static inline uint8 hp_inline_hash(char * str);
static double get_timebase_factor();
static long get_us_interval(struct timeval *start, struct timeval *end);
static inline double get_us_from_tsc(uint64 count);

static void hp_parse_options_from_arg(zval *args);
static void hp_clean_profiler_options_state();

static void hp_exception_function_clear();
static void hp_transaction_function_clear();
static void hp_transaction_name_clear();

static inline zval  *hp_zval_at_key(char  *key, zval  *values);
static inline char **hp_strings_in_zval(zval  *values);
static inline void   hp_array_del(char **name_array);
static inline hp_string *hp_create_string(const char *value, size_t length);
static inline long hp_zval_to_long(zval *z);
static inline hp_string *hp_zval_to_string(zval *z);
static inline zval *hp_string_to_zval(hp_string *str);
static inline void hp_string_clean(hp_string *str);
static char *hp_get_file_summary(char *filename, int filename_len TSRMLS_DC);
static char *hp_get_base_filename(char *filename);

static inline hp_function_map *hp_function_map_create(char **names);
static inline void hp_function_map_clear(hp_function_map *map);
static inline int hp_function_map_exists(hp_function_map *map, uint8 hash_code, char *curr_func);
static inline int hp_function_map_filter_collision(hp_function_map *map, uint8 hash);

/* {{{ arginfo */
ZEND_BEGIN_ARG_INFO_EX(arginfo_tideways_enable, 0, 0, 0)
  ZEND_ARG_INFO(0, flags)
  ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_tideways_disable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_tideways_transaction_name, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_tideways_prepend_overwritten, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_tideways_fatal_backtrace, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_tideways_last_detected_exception, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_tideways_last_fatal_error, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_tideways_sql_minify, 0, 0, 0)
	ZEND_ARG_INFO(0, sql)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_tideways_span_create, 0, 0, 0)
	ZEND_ARG_INFO(0, category)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_tideways_get_spans, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_tideways_span_timer_start, 0, 0, 0)
	ZEND_ARG_INFO(0, span)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_tideways_span_timer_stop, 0, 0, 0)
	ZEND_ARG_INFO(0, span)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_tideways_span_annotate, 0, 0, 0)
	ZEND_ARG_INFO(0, span)
	ZEND_ARG_INFO(0, annotations)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_tideways_span_watch, 0, 0, 0)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, category)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_tideways_span_callback, 0, 0, 0)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, callback)
ZEND_END_ARG_INFO()

/* }}} */

/**
 * *********************
 * PHP EXTENSION GLOBALS
 * *********************
 */
/* List of functions implemented/exposed by Tideways */
zend_function_entry tideways_functions[] = {
	PHP_FE(tideways_enable, arginfo_tideways_enable)
	PHP_FE(tideways_disable, arginfo_tideways_disable)
	PHP_FE(tideways_transaction_name, arginfo_tideways_transaction_name)
	PHP_FE(tideways_prepend_overwritten, arginfo_tideways_prepend_overwritten)
	PHP_FE(tideways_fatal_backtrace, arginfo_tideways_fatal_backtrace)
	PHP_FE(tideways_last_detected_exception, arginfo_tideways_last_detected_exception)
	PHP_FE(tideways_last_fatal_error, arginfo_tideways_last_fatal_error)
	PHP_FE(tideways_sql_minify, arginfo_tideways_sql_minify)
	PHP_FE(tideways_span_create, arginfo_tideways_span_create)
	PHP_FE(tideways_get_spans, arginfo_tideways_get_spans)
	PHP_FE(tideways_span_timer_start, arginfo_tideways_span_timer_start)
	PHP_FE(tideways_span_timer_stop, arginfo_tideways_span_timer_stop)
	PHP_FE(tideways_span_annotate, arginfo_tideways_span_annotate)
	PHP_FE(tideways_span_watch, arginfo_tideways_span_watch)
	PHP_FE(tideways_span_callback, arginfo_tideways_span_callback)
	{NULL, NULL, NULL}
};

/* Callback functions for the Tideways extension */
zend_module_entry tideways_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	"tideways",                        /* Name of the extension */
	tideways_functions,                /* List of functions exposed */
	PHP_MINIT(tideways),               /* Module init callback */
	PHP_MSHUTDOWN(tideways),           /* Module shutdown callback */
	PHP_RINIT(tideways),               /* Request init callback */
	PHP_RSHUTDOWN(tideways),           /* Request shutdown callback */
	PHP_MINFO(tideways),               /* Module info callback */
#if ZEND_MODULE_API_NO >= 20010901
	TIDEWAYS_VERSION,
#endif
	STANDARD_MODULE_PROPERTIES
};

PHP_INI_BEGIN()

/**
 * INI-Settings are always used by the extension, but by the PHP library.
 */
PHP_INI_ENTRY("tideways.connection", "unix:///var/run/tideways/tidewaysd.sock", PHP_INI_ALL, NULL)
PHP_INI_ENTRY("tideways.udp_connection", "127.0.0.1:8135", PHP_INI_ALL, NULL)
PHP_INI_ENTRY("tideways.auto_start", "1", PHP_INI_ALL, NULL)
PHP_INI_ENTRY("tideways.api_key", "", PHP_INI_ALL, NULL)
PHP_INI_ENTRY("tideways.framework", "", PHP_INI_ALL, NULL)
PHP_INI_ENTRY("tideways.sample_rate", "30", PHP_INI_ALL, NULL)
PHP_INI_ENTRY("tideways.auto_prepend_library", "1", PHP_INI_ALL, NULL)
PHP_INI_ENTRY("tideways.collect", "tracing", PHP_INI_ALL, NULL)
PHP_INI_ENTRY("tideways.monitor", "basic", PHP_INI_ALL, NULL)
PHP_INI_ENTRY("tideways.distributed_tracing_hosts", "127.0.0.1", PHP_INI_ALL, NULL)

PHP_INI_END()

/* Init module */
ZEND_GET_MODULE(tideways)


/**
 * **********************************
 * PHP EXTENSION FUNCTION DEFINITIONS
 * **********************************
 */

/**
 * Start Tideways profiling in hierarchical mode.
 *
 * @param  long $flags  flags for hierarchical mode
 * @return void
 * @author kannan
 */
PHP_FUNCTION(tideways_enable)
{
	long tideways_flags = 0;
	zval *optional_array = NULL;

	if (hp_globals.enabled) {
		hp_stop(TSRMLS_C);
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
				"|lz", &tideways_flags, &optional_array) == FAILURE) {
		return;
	}

	hp_parse_options_from_arg(optional_array);

	hp_begin(tideways_flags TSRMLS_CC);
}

/**
 * Stops Tideways from profiling  and returns the profile info.
 *
 * @param  void
 * @return array  hash-array of Tideways's profile info
 * @author cjiang
 */
PHP_FUNCTION(tideways_disable)
{
	if (!hp_globals.enabled) {
		return;
	}

	hp_stop(TSRMLS_C);

	RETURN_ZVAL(hp_globals.stats_count, 1, 0);
}

PHP_FUNCTION(tideways_transaction_name)
{
	if (hp_globals.transaction_name) {
		zval *ret = hp_string_to_zval(hp_globals.transaction_name);
		RETURN_ZVAL(ret, 1, 1);
	}
}

PHP_FUNCTION(tideways_prepend_overwritten)
{
	RETURN_BOOL(hp_globals.prepend_overwritten);
}

PHP_FUNCTION(tideways_fatal_backtrace)
{
	if (hp_globals.backtrace != NULL) {
		RETURN_ZVAL(hp_globals.backtrace, 1, 1);
	}
}

PHP_FUNCTION(tideways_last_detected_exception)
{
	if (hp_globals.exception != NULL) {
		RETURN_ZVAL(hp_globals.exception, 1, 0);
	}
}

PHP_FUNCTION(tideways_last_fatal_error)
{
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	if (PG(last_error_message)) {
		array_init(return_value);
		add_assoc_long_ex(return_value, "type", sizeof("type"), PG(last_error_type));
		add_assoc_string_ex(return_value, "message", sizeof("message"), PG(last_error_message), 1);
		add_assoc_string_ex(return_value, "file", sizeof("file"), PG(last_error_file)?PG(last_error_file):"-", 1 );
		add_assoc_long_ex(return_value, "line", sizeof("line"), PG(last_error_lineno));
	}
}

long tw_span_create(char *category, size_t category_len)
{
	zval *span, *starts, *stops, *annotations;
	int idx;
	long parent = 0;

	idx = zend_hash_num_elements(Z_ARRVAL_P(hp_globals.spans));

	// Hardcode a limit of 1500 spans for now, Daemon will re-filter again to 1000.
	// We assume web-requests and non-spammy worker/crons here, need a way to support
	// very long running scripts at some point.
	if (idx >= 1500) {
		return -1;
	}

	MAKE_STD_ZVAL(span);
	MAKE_STD_ZVAL(starts);
	MAKE_STD_ZVAL(stops);

	array_init(span);
	array_init(starts);
	array_init(stops);

	add_assoc_stringl(span, "n", category, category_len, 1);
	add_assoc_zval(span, "b", starts);
	add_assoc_zval(span, "e", stops);

	if (parent > 0) {
		add_assoc_long(span, "p", parent);
	}

	zend_hash_index_update(Z_ARRVAL_P(hp_globals.spans), idx, &span, sizeof(zval*), NULL);

	return idx;
}

void tw_span_timer_start(long spanId)
{
	zval **span, **starts;
	double wt;

	if (zend_hash_index_find(Z_ARRVAL_P(hp_globals.spans), spanId, (void **) &span) == FAILURE) {
		return;
	}

	if (zend_hash_find(Z_ARRVAL_PP(span), "b", sizeof("b"), (void **) &starts) == FAILURE) {
		return;
	}

	wt = get_us_from_tsc(cycle_timer() - hp_globals.start_time);
	add_next_index_long(*starts, wt);
}

void tw_span_record_duration(long spanId, double start, double end)
{
	zval **span, **timer;

	if (zend_hash_index_find(Z_ARRVAL_P(hp_globals.spans), spanId, (void **) &span) == FAILURE) {
		return;
	}

	if (zend_hash_find(Z_ARRVAL_PP(span), "e", sizeof("e"), (void **) &timer) == FAILURE) {
		return;
	}

	add_next_index_long(*timer, end);

	if (zend_hash_find(Z_ARRVAL_PP(span), "b", sizeof("b"), (void **) &timer) == FAILURE) {
		return;
	}

	add_next_index_long(*timer, start);
}

void tw_span_timer_stop(long spanId)
{
	zval **span, **stops;
	double wt;

	if (zend_hash_index_find(Z_ARRVAL_P(hp_globals.spans), spanId, (void **) &span) == FAILURE) {
		return;
	}

	if (zend_hash_find(Z_ARRVAL_PP(span), "e", sizeof("e"), (void **) &stops) == FAILURE) {
		return;
	}

	wt = get_us_from_tsc(cycle_timer() - hp_globals.start_time);
	add_next_index_long(*stops, wt);
}

static int tw_convert_to_string(void *pDest TSRMLS_DC)
{
	zval **zv = (zval **) pDest;

	convert_to_string_ex(zv);

	return ZEND_HASH_APPLY_KEEP;
}

void tw_span_annotate(long spanId, zval *annotations TSRMLS_DC)
{
	zval **span, **span_annotations, *span_annotations_ptr;

	if (zend_hash_index_find(Z_ARRVAL_P(hp_globals.spans), spanId, (void **) &span) == FAILURE) {
		return;
	}

	if (zend_hash_find(Z_ARRVAL_PP(span), "a", sizeof("a"), (void **) &span_annotations) == FAILURE) {
		MAKE_STD_ZVAL(span_annotations_ptr);
		array_init(span_annotations_ptr);
		span_annotations = &span_annotations_ptr;
		add_assoc_zval(*span, "a", span_annotations_ptr);
	}

	zend_hash_apply(Z_ARRVAL_P(annotations), tw_convert_to_string TSRMLS_CC);

	zend_hash_merge(Z_ARRVAL_PP(span_annotations), Z_ARRVAL_P(annotations), (copy_ctor_func_t) zval_add_ref, NULL, sizeof(zval *), 1);
}

void tw_span_annotate_long(long spanId, char *key, long value)
{
	zval **span, **span_annotations, *annotation_value, *span_annotations_ptr;

	if (zend_hash_index_find(Z_ARRVAL_P(hp_globals.spans), spanId, (void **) &span) == FAILURE) {
		return;
	}

	if (zend_hash_find(Z_ARRVAL_PP(span), "a", sizeof("a"), (void **) &span_annotations) == FAILURE) {
		MAKE_STD_ZVAL(span_annotations_ptr);
		array_init(span_annotations_ptr);
		span_annotations = &span_annotations_ptr;
		add_assoc_zval(*span, "a", span_annotations_ptr);
	}

	MAKE_STD_ZVAL(annotation_value);
	ZVAL_LONG(annotation_value, value);
	convert_to_string_ex(&annotation_value);

	add_assoc_zval_ex(*span_annotations, key, strlen(key)+1, annotation_value);
}

void tw_span_annotate_string(long spanId, char *key, char *value, int copy)
{
	zval **span, **span_annotations, *span_annotations_ptr;
	int len;

	if (zend_hash_index_find(Z_ARRVAL_P(hp_globals.spans), spanId, (void **) &span) == FAILURE) {
		return;
	}

	if (zend_hash_find(Z_ARRVAL_PP(span), "a", sizeof("a"), (void **) &span_annotations) == FAILURE) {
		MAKE_STD_ZVAL(span_annotations_ptr);
		array_init(span_annotations_ptr);
		span_annotations = &span_annotations_ptr;
		add_assoc_zval(*span, "a", span_annotations_ptr);
	}

	// limit size of annotations to 1000 characters, this mostly affects "sql"
	// annotations, but the daemon sql parser is resilent against broken SQL.
	len = strlen(value);
	if (copy == 1 && len > 1000) {
		len = 1000;
	}

	add_assoc_stringl_ex(*span_annotations, key, strlen(key)+1, value, len, copy);
}

PHP_FUNCTION(tideways_span_create)
{
	char *category = NULL;
	size_t category_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &category, &category_len) == FAILURE) {
		return;
	}

	if (hp_globals.enabled == 0) {
		return;
	}

	RETURN_LONG(tw_span_create(category, category_len));
}

PHP_FUNCTION(tideways_get_spans)
{
	if (hp_globals.spans) {
		RETURN_ZVAL(hp_globals.spans, 1, 0);
	}
}

PHP_FUNCTION(tideways_span_timer_start)
{
	long spanId;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &spanId) == FAILURE) {
		return;
	}

	if (hp_globals.enabled == 0) {
		return;
	}

	tw_span_timer_start(spanId);
}

PHP_FUNCTION(tideways_span_timer_stop)
{
	long spanId;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &spanId) == FAILURE) {
		return;
	}

	if (hp_globals.enabled == 0) {
		return;
	}

	tw_span_timer_stop(spanId);
}

PHP_FUNCTION(tideways_span_annotate)
{
	long spanId;
	zval *annotations;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "lz", &spanId, &annotations) == FAILURE) {
		return;
	}

	// Yes, annotations are still possible when profiler is deactivated!
	tw_span_annotate(spanId, annotations TSRMLS_CC);
}

PHP_FUNCTION(tideways_sql_minify)
{
	RETURN_EMPTY_STRING();
}

/**
 * Module init callback.
 *
 * @author cjiang
 */
PHP_MINIT_FUNCTION(tideways)
{
	int i;

	REGISTER_INI_ENTRIES();

	hp_register_constants(INIT_FUNC_ARGS_PASSTHRU);

	/* Get the number of available logical CPUs. */
	hp_globals.timebase_factor = get_timebase_factor();

	hp_globals.stats_count = NULL;
	hp_globals.spans = NULL;
	hp_globals.trace_callbacks = NULL;
	hp_globals.trace_watch_callbacks = NULL;
	hp_globals.span_cache = NULL;

	/* no free hp_entry_t structures to start with */
	hp_globals.entry_free_list = NULL;

	for (i = 0; i < 256; i++) {
		hp_globals.func_hash_counters[i] = 0;
	}

	hp_transaction_function_clear();
	hp_exception_function_clear();

#if defined(DEBUG)
	/* To make it random number generator repeatable to ease testing. */
	srand(0);
#endif
	return SUCCESS;
}

/**
 * Module shutdown callback.
 */
PHP_MSHUTDOWN_FUNCTION(tideways)
{
	/* free any remaining items in the free list */
	hp_free_the_free_list();

	UNREGISTER_INI_ENTRIES();

	return SUCCESS;
}

long tw_trace_callback_record_with_cache(char *category, int category_len, char *summary, int summary_len, int copy)
{
	long idx, *idx_ptr;

	if (zend_hash_find(hp_globals.span_cache, summary, strlen(summary)+1, (void **)&idx_ptr) == SUCCESS) {
		idx = *idx_ptr;
	} else {
		idx = tw_span_create(category, category_len);
		zend_hash_update(hp_globals.span_cache, summary, strlen(summary)+1, &idx, sizeof(long), NULL);
	}

	tw_span_annotate_string(idx, "title", summary, copy);

	return idx;
}

long tw_trace_callback_php_call(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	long idx;

	idx = tw_span_create("php", 3);
	tw_span_annotate_string(idx, "title", symbol, 1);

	return idx;
}

long tw_trace_callback_watch(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	tw_watch_callback **temp;
	tw_watch_callback *twcb;
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcic = empty_fcall_info_cache;

	if (hp_globals.trace_watch_callbacks == NULL) {
		return -1;
	}

	if (zend_hash_find(hp_globals.trace_watch_callbacks, symbol, strlen(symbol)+1, (void **)&temp) == SUCCESS) {
		zval *retval = NULL;
		zval *context = NULL;
		zval *zargs = NULL;
		zval *params[1];
		zend_error_handling zeh;
		twcb = *temp;
		int i;

		MAKE_STD_ZVAL(context);
		array_init(context);

		MAKE_STD_ZVAL(zargs);
		array_init(zargs);
		Z_ADDREF_P(zargs);

		add_assoc_string_ex(context, "fn", sizeof("fn"), symbol, 1);

		if (args_len > 0) {
			for (i = 0; i < args_len; i++) {
				Z_ADDREF_P(*(args-args_len+i));
				add_next_index_zval(zargs, *(args-args_len+i));
			}
		}

		add_assoc_zval(context, "args", zargs);

		if (object != NULL) {
			Z_ADDREF_P(object);
			add_assoc_zval(context, "object", object);
		}

		params[0] = (zval *)&(context);

		twcb->fci.param_count = 1;
		twcb->fci.size = sizeof(twcb->fci);
		twcb->fci.retval_ptr_ptr = &retval;
		twcb->fci.params = (zval ***)params;

		if (zend_call_function(&(twcb->fci), &(twcb->fcic) TSRMLS_CC) == FAILURE) {
			zend_error(E_ERROR, "Cannot call Trace Watch Callback");
		}

		zval_ptr_dtor(&context);
		zval_ptr_dtor(&zargs);

		long idx = -1;

		if (retval) {
			if (Z_TYPE_P(retval) == IS_LONG) {
				idx = Z_LVAL_P(retval);
			}

			zval_ptr_dtor(&retval);
		}

		return idx;
	}

	return -1;
}

long tw_trace_callback_mongo_cursor_io(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	long idx = -1;
	zval fname, *retval_ptr, **data;

	idx = tw_span_create("mongo", 5);
	tw_span_annotate_string(idx, "title", symbol, 1);

	ZVAL_STRING(&fname, "info", 0);

	if (SUCCESS == call_user_function_ex(EG(function_table), &object, &fname, &retval_ptr, 0, NULL, 1, NULL TSRMLS_CC)) {
		if (Z_TYPE_P(retval_ptr) == IS_ARRAY) {
			if (zend_hash_find(Z_ARRVAL_P(retval_ptr), "ns", sizeof("ns"), (void**)&data) == SUCCESS) {
				tw_span_annotate_string(idx, "collection", Z_STRVAL_PP(data), 1);
			}
		}

		zval_ptr_dtor(&retval_ptr);
	}

	return idx;
}

long tw_trace_callback_mongo_cursor_next(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	long idx = -1;
	zend_class_entry *cursor_ce;
	zval *queryRunProperty;
	zval fname, *retval_ptr, **data;

	if (object == NULL || Z_TYPE_P(object) != IS_OBJECT) {
		return idx;
	}

	cursor_ce = Z_OBJCE_P(object);
	queryRunProperty = zend_read_property(cursor_ce, object, "_tidewaysQueryRun", sizeof("_tidewaysQueryRun")-1, 1 TSRMLS_CC);

	if (queryRunProperty != NULL && Z_TYPE_P(queryRunProperty) != IS_NULL) {
		return idx;
	}

	zend_update_property_bool(cursor_ce, object, "_tidewaysQueryRun", sizeof("_tidewaysQueryRun") - 1, 1 TSRMLS_CC);

	idx = tw_span_create("mongo", 5);
	tw_span_annotate_string(idx, "title", symbol, 1);

	ZVAL_STRING(&fname, "info", 0);

	if (SUCCESS == call_user_function_ex(EG(function_table), &object, &fname, &retval_ptr, 0, NULL, 1, NULL TSRMLS_CC)) {
		if (Z_TYPE_P(retval_ptr) == IS_ARRAY) {
			if (zend_hash_find(Z_ARRVAL_P(retval_ptr), "ns", sizeof("ns"), (void**)&data) == SUCCESS) {
				tw_span_annotate_string(idx, "collection", Z_STRVAL_PP(data), 1);
			}
		}

		zval_ptr_dtor(&retval_ptr);
	}

	return idx;
}

long tw_trace_callback_mongo_collection(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	long idx = -1;
	zval fname, *retval_ptr;

	if (object == NULL || Z_TYPE_P(object) != IS_OBJECT) {
		return idx;
	}

	ZVAL_STRING(&fname, "getName", 0);

	idx = tw_span_create("mongo", 5);
	tw_span_annotate_string(idx, "title", symbol, 1);

	if (SUCCESS == call_user_function_ex(EG(function_table), &object, &fname, &retval_ptr, 0, NULL, 1, NULL TSRMLS_CC)) {
		if (Z_TYPE_P(retval_ptr) == IS_STRING) {
			tw_span_annotate_string(idx, "collection", Z_STRVAL_P(retval_ptr), 1);
		}

		zval_ptr_dtor(&retval_ptr);
	}

	return idx;
}

long tw_trace_callback_predis_call(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *commandId = *(args-args_len);

	if (commandId == NULL || Z_TYPE_P(commandId) != IS_STRING) {
		return -1;
	}

	return tw_trace_callback_record_with_cache("predis", 6, Z_STRVAL_P(commandId), Z_STRLEN_P(commandId), 1);
}

long tw_trace_callback_phpampqlib(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *exchange;
	long idx = -1;

	if (args_len < 2) {
		return idx;
	}

	exchange = *(args-args_len+1);

	if (exchange == NULL || Z_TYPE_P(exchange) != IS_STRING) {
		return idx;
	}

	return tw_trace_callback_record_with_cache("queue", 5, Z_STRVAL_P(exchange), Z_STRLEN_P(exchange), 1);
}

long tw_trace_callback_pheanstalk(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zend_class_entry *pheanstalk_ce;
	zval *property;
	long idx = -1;

	if (Z_TYPE_P(object) != IS_OBJECT) {
		return idx;
	}

	pheanstalk_ce = Z_OBJCE_P(object);

	property = zend_read_property(pheanstalk_ce, object, "_using", sizeof("_using") - 1, 1 TSRMLS_CC);

	if (property != NULL && Z_TYPE_P(property) == IS_STRING) {
		return tw_trace_callback_record_with_cache("queue", 5, Z_STRVAL_P(property), Z_STRLEN_P(property), 1);
	} else {
		return tw_trace_callback_record_with_cache("queue", 5, "default", 7, 1);
	}
}

long tw_trace_callback_memcache(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	return tw_trace_callback_record_with_cache("memcache", 8, symbol, strlen(symbol), 1);
}

long tw_trace_callback_php_controller(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	long idx;

	idx = tw_span_create("php.ctrl", 8);
	tw_span_annotate_string(idx, "title", symbol, 1);

	return idx;
}

long tw_trace_callback_doctrine_couchdb_request(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *method = *(args-args_len);
	zval *path = *(args-args_len+1);
	long idx;

	if (Z_TYPE_P(method) != IS_STRING || Z_TYPE_P(path) != IS_STRING) {
		return -1;
	}

	idx = tw_span_create("http", 4);
	tw_span_annotate_string(idx, "method", Z_STRVAL_P(method), 1);
	tw_span_annotate_string(idx, "url", Z_STRVAL_P(path), 1);
	tw_span_annotate_string(idx, "service", "couchdb", 1);

	return idx;
}

/* Mage_Core_Block_Abstract::toHtml() */
long tw_trace_callback_magento_block(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zend_class_entry *ce;

	ce = Z_OBJCE_P(object);

	return tw_trace_callback_record_with_cache("view", 4, (char*)ce->name, ce->name_length, 1);
}

/* Zend_View_Abstract::render($name); */
long tw_trace_callback_view_engine(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *name = *(args-args_len);
	char *view;

	if (Z_TYPE_P(name) != IS_STRING) {
		return -1;
	}

	view = hp_get_base_filename(Z_STRVAL_P(name));

	return tw_trace_callback_record_with_cache("view", 4, view, strlen(view)+1, 1);
}

/* Applies to Enlight, Mage and Zend1 */
long tw_trace_callback_zend1_dispatcher_families_tx(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *argument_element = *(args-args_len);
	int len;
	char *ret = NULL;
	zend_class_entry *ce;
	tw_trace_callback *cb;
	long idx;

	if (Z_TYPE_P(argument_element) != IS_STRING) {
		return -1;
	}

	ce = Z_OBJCE_P(object);

	len = ce->name_length + Z_STRLEN_P(argument_element) + 3;
	ret = (char*)emalloc(len);
	snprintf(ret, len, "%s::%s", ce->name, Z_STRVAL_P(argument_element));

	idx = tw_span_create("php.ctrl", 8);
	tw_span_annotate_string(idx, "title", ret, 0);

	return idx;
}

/* oxShopControl::_process($sClass, $sFnc = null); */
long tw_trace_callback_oxid_tx(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *sClass = *(args-args_len);
	zval *sFnc = *(args-args_len+1);
	char *ret = NULL;
	int len, copy;

	if (Z_TYPE_P(sClass) != IS_STRING) {
		return -1;
	}

	if (args_len > 1 && sFnc != NULL && Z_TYPE_P(sFnc) == IS_STRING) {
		len = Z_STRLEN_P(sClass) + Z_STRLEN_P(sFnc) + 3;
		ret = (char*)emalloc(len);
		snprintf(ret, len, "%s::%s", Z_STRVAL_P(sClass), Z_STRVAL_P(sFnc));
		copy = 0;
	} else {
		ret = Z_STRVAL_P(sClass);
		len = Z_STRLEN_P(sClass);
		copy = 1;
	}

	if ((hp_globals.tideways_flags & TIDEWAYS_FLAGS_NO_SPANS) > 0) {
		return -1;
	}

	return tw_trace_callback_record_with_cache("php.ctrl", 8, ret, len, copy);
}

/* $resolver->getArguments($request, $controller); */
long tw_trace_callback_symfony_resolve_arguments_tx(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *callback, **controller, **action;
	const char *class_name;
	zend_uint class_name_len;
	const char *free_class_name = NULL;
	char *ret = NULL;
	int len;
	tw_trace_callback cb;

	callback = *(args-args_len+1);

	// Only Symfony2 framework for now
	if (Z_TYPE_P(callback) == IS_ARRAY) {
		if (zend_hash_index_find(Z_ARRVAL_P(callback), 0, (void**)&controller) == FAILURE) {
			return -1;
		}

		if (Z_TYPE_PP(controller) != IS_OBJECT) {
			return -1;
		}

		if (zend_hash_index_find(Z_ARRVAL_P(callback), 1, (void**)&action) == FAILURE) {
			return -1;
		}

		if (Z_TYPE_PP(action) != IS_STRING) {
			return -1;
		}

		if (!zend_get_object_classname(*controller, &class_name, &class_name_len TSRMLS_CC)) {
			free_class_name = class_name;
		}

		len = class_name_len + Z_STRLEN_PP(action) + 3;
		ret = (char*)emalloc(len);
		snprintf(ret, len, "%s::%s", class_name, Z_STRVAL_PP(action));

		cb = tw_trace_callback_php_controller;
		register_trace_callback_len(ret, len-1, cb);

		if (free_class_name) {
			efree((char*)free_class_name);
		}
		efree(ret);
	}

	return -1;
}

long tw_trace_callback_pgsql_execute(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *argument_element;
	char *summary;
	int i;

	for (i = 0; i < args_len; i++) {
		argument_element = *(args-(args_len-i));

		if (argument_element && Z_TYPE_P(argument_element) == IS_STRING && Z_STRLEN_P(argument_element) > 0) {
			// TODO: Introduce SQL statement cache to find the names here again.
			summary = Z_STRVAL_P(argument_element);

			return tw_trace_callback_record_with_cache("sql", 3, summary, strlen(summary), 1);
		}
	}

	return -1;
}

long tw_trace_callback_pgsql_query(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *argument_element;
	long idx;
	int i;

	for (i = 0; i < args_len; i++) {
		argument_element = *(args-(args_len-i));

		if (argument_element && Z_TYPE_P(argument_element) == IS_STRING) {
			idx = tw_span_create("sql", 3);
			tw_span_annotate_string(idx, "sql", Z_STRVAL_P(argument_element), 1);

			return idx;
		}
	}

	return -1;
}

long tw_trace_callback_smarty3_template(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *argument_element = *(args-args_len);
	zval *obj;
	zend_class_entry *smarty_ce;
	char *template;
	size_t template_len;

	if (argument_element && Z_TYPE_P(argument_element) == IS_STRING) {
		template = Z_STRVAL_P(argument_element);
	} else {
		smarty_ce = Z_OBJCE_P(object);

		argument_element = zend_read_property(smarty_ce, object, "template_resource", sizeof("template_resource") - 1, 1 TSRMLS_CC);

		if (Z_TYPE_P(argument_element) != IS_STRING) {
			return -1;
		}

		template = Z_STRVAL_P(argument_element);
	}

	template_len = Z_STRLEN_P(argument_element);

	return tw_trace_callback_record_with_cache("view", 4, template, template_len, 1);
}

long tw_trace_callback_doctrine_persister(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *property;
	zend_class_entry *persister_ce, *metadata_ce;

	persister_ce = Z_OBJCE_P(object);

	property = zend_read_property(persister_ce, object, "class", sizeof("class") - 1, 1 TSRMLS_CC);
	if (property == NULL) {
		property = zend_read_property(persister_ce, object, "_class", sizeof("_class") - 1, 1 TSRMLS_CC);
	}

	if (property != NULL && Z_TYPE_P(property) == IS_OBJECT) {
		metadata_ce = Z_OBJCE_P(property);

		property = zend_read_property(metadata_ce, property, "name", sizeof("name") - 1, 1 TSRMLS_CC);

		if (property == NULL) {
			return -1;
		}

		return tw_trace_callback_record_with_cache("doctrine.load", 13, Z_STRVAL_P(property), Z_STRLEN_P(property), 1);
	}

	return -1;
}

long tw_trace_callback_doctrine_query(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *property, **tmp;
	zend_class_entry *query_ce, *rsm_ce;
	zval fname, *retval_ptr;
	char *summary;
	long idx = -1;
	HashPosition pos;

	if (object == NULL || Z_TYPE_P(object) != IS_OBJECT) {
		return idx;
	}

	idx = tw_span_create("doctrine.query", 14);

	query_ce = Z_OBJCE_P(object);

	property = zend_read_property(query_ce, object, "_resultSetMapping", sizeof("_resultSetMapping") - 1, 1 TSRMLS_CC);

	if (property == NULL) {
		property = zend_read_property(query_ce, object, "resultSetMapping", sizeof("resultSetMapping") - 1, 1 TSRMLS_CC);
	}

	if (property == NULL || Z_TYPE_P(property) != IS_OBJECT) {
		return idx;
	}

	rsm_ce = Z_OBJCE_P(property);
	property = zend_read_property(rsm_ce, property, "aliasMap", sizeof("aliasMap")-1, 1 TSRMLS_CC);

	if (property == NULL || Z_TYPE_P(property) != IS_ARRAY) {
		return idx;
	}

	if (zend_hash_num_elements(Z_ARRVAL_P(property)) == 0) {
		return idx;
	}

	zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(property), &pos);

	if (zend_hash_get_current_data_ex(Z_ARRVAL_P(property), (void **) &tmp, &pos) == SUCCESS) {
		if (Z_TYPE_P(*tmp) == IS_STRING) {
			tw_span_annotate_string(idx, "title", Z_STRVAL_P(*tmp), 1);
		}
	}

	return idx;
}

long tw_trace_callback_twig_template(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	long idx = -1, *idx_ptr;
	zval fname, *retval_ptr;

	if (object == NULL || Z_TYPE_P(object) != IS_OBJECT) {
		return idx;
	}

	ZVAL_STRING(&fname, "getTemplateName", 0);

	if (SUCCESS == call_user_function_ex(EG(function_table), &object, &fname, &retval_ptr, 0, NULL, 1, NULL TSRMLS_CC)) {
		if (Z_TYPE_P(retval_ptr) == IS_STRING) {
			idx = tw_trace_callback_record_with_cache("view", 4, Z_STRVAL_P(retval_ptr), Z_STRLEN_P(retval_ptr), 1);
		}

		zval_ptr_dtor(&retval_ptr);
	}

	return idx;
}

long tw_trace_callback_event_dispatchers(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	long idx = -1, *idx_ptr;
	zval *argument_element = *(args-args_len);

	if (argument_element && Z_TYPE_P(argument_element) == IS_STRING) {
		idx = tw_trace_callback_record_with_cache("event", 5, Z_STRVAL_P(argument_element), Z_STRLEN_P(argument_element), 1);
	}

	return idx;
}

long tw_trace_callback_pdo_stmt_execute(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	long idx;

	pdo_stmt_t *stmt = (pdo_stmt_t*)zend_object_store_get_object_by_handle(Z_OBJ_HANDLE_P(object) TSRMLS_CC);
	idx = tw_span_create("sql", 3);
	tw_span_annotate_string(idx, "sql", stmt->query_string, 1);

	return idx;
}

long tw_trace_callback_mysqli_stmt_execute(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	return tw_trace_callback_record_with_cache("sql", 3, "execute", 7, 1);
}

long tw_trace_callback_sql_commit(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	return tw_trace_callback_record_with_cache("sql", 3, "commit", 3, 1);
}

long tw_trace_callback_sql_functions(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *argument_element;
	long idx;

	if (strcmp(symbol, "mysqli_query") == 0 || strcmp(symbol, "mysqli_prepare") == 0) {
		argument_element = *(args-args_len+1);
	} else {
		argument_element = *(args-args_len);
	}

	if (Z_TYPE_P(argument_element) != IS_STRING) {
		return -1;
	}

	idx = tw_span_create("sql", 3);
	tw_span_annotate_string(idx, "sql", Z_STRVAL_P(argument_element), 1);

	return idx;
}

long tw_trace_callback_fastcgi_finish_request(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	// stop the main span, the request ended here
	tw_span_timer_stop(0);
	return -1;
}

long tw_trace_callback_curl_exec(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *argument = *(args-args_len);
	zval **option;
	zval ***params_array;
	char *summary;
	long idx;
	zval fname, *retval_ptr, *opt;

	if (argument == NULL || Z_TYPE_P(argument) != IS_RESOURCE) {
		return -1;
	}

	ZVAL_STRING(&fname, "curl_getinfo", 0);

	params_array = (zval ***) emalloc(sizeof(zval **));
	params_array[0] = &argument;

	if (SUCCESS == call_user_function_ex(EG(function_table), NULL, &fname, &retval_ptr, 1, params_array, 1, NULL TSRMLS_CC)) {
		if (zend_hash_find(Z_ARRVAL_P(retval_ptr), "url", sizeof("url"), (void **)&option) == SUCCESS) {
			summary = hp_get_file_summary(Z_STRVAL_PP(option), Z_STRLEN_PP(option) TSRMLS_CC);

			efree(params_array);
			zval_ptr_dtor(&retval_ptr);

			idx = tw_span_create("http", 4);
			tw_span_annotate_string(idx, "url", summary, 0);
			return idx;
		}

		zval_ptr_dtor(&retval_ptr);
	}

	efree(params_array);

	return -1;
}

long tw_trace_callback_soap_client_dorequest(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	if (args_len < 2) {
		return -1;
	}

	long idx = -1;
	zval *argument = *(args-args_len+1);

	if (Z_TYPE_P(argument) != IS_STRING) {
		return idx;
	}

	idx = tw_span_create("http", 4);
	tw_span_annotate_string(idx, "url", Z_STRVAL_P(argument), 1);
	tw_span_annotate_string(idx, "method", "POST", 1);
	tw_span_annotate_string(idx, "service", "soap", 1);

	return idx;
}

long tw_trace_callback_file_get_contents(char *symbol, void **args, int args_len, zval *object TSRMLS_DC)
{
	zval *argument = *(args-args_len);
	long idx = -1;

	if (Z_TYPE_P(argument) != IS_STRING) {
		return idx;
	}

	if (strncmp(Z_STRVAL_P(argument), "http", 4) != 0) {
		return idx;
	}

	idx = tw_span_create("http", 4);
	tw_span_annotate_string(idx, "url", Z_STRVAL_P(argument), 1);

	return idx;
}

/**
 * Request init callback.
 *
 * Check if Tideways.php exists in extension_dir and load it
 * in request init. This makes class \Tideways\Profiler available
 * for usage.
 */
PHP_RINIT_FUNCTION(tideways)
{
	char *extension_dir;
	char *profiler_file;
	int profiler_file_len;

	hp_globals.prepend_overwritten = 0;
	hp_globals.backtrace = NULL;
	hp_globals.exception = NULL;

	if (INI_INT("tideways.auto_prepend_library") == 0) {
		return SUCCESS;
	}

	extension_dir  = INI_STR("extension_dir");
	profiler_file_len = strlen(extension_dir) + strlen("Tideways.php") + 2;
	profiler_file = emalloc(profiler_file_len);
	snprintf(profiler_file, profiler_file_len, "%s/%s", extension_dir, "Tideways.php");

	if (PG(open_basedir) && php_check_open_basedir_ex(profiler_file, 0 TSRMLS_CC)) {
		efree(profiler_file);
		return SUCCESS;
	}

	if (VCWD_ACCESS(profiler_file, F_OK) == 0) {
		PG(auto_prepend_file) = profiler_file;
		hp_globals.prepend_overwritten = 1;
	} else {
		efree(profiler_file);
	}

	return SUCCESS;
}

/**
 * Request shutdown callback. Stop profiling and return.
 */
PHP_RSHUTDOWN_FUNCTION(tideways)
{
	hp_end(TSRMLS_C);

	if (hp_globals.prepend_overwritten == 1) {
		efree(PG(auto_prepend_file));
		PG(auto_prepend_file) = NULL;
		hp_globals.prepend_overwritten = 0;
	}

	return SUCCESS;
}

/**
 * Module info callback. Returns the Tideways version.
 */
PHP_MINFO_FUNCTION(tideways)
{
	char *extension_dir;
	char *profiler_file;
	int profiler_file_len;

	php_info_print_table_start();
	php_info_print_table_header(2, "tideways", TIDEWAYS_VERSION);

	php_info_print_table_row(2, "Connection (tideways.connection)", INI_STR("tideways.connection"));
	php_info_print_table_row(2, "UDP Connection (tideways.udp_connection)", INI_STR("tideways.udp_connection"));
	php_info_print_table_row(2, "Default API Key (tideways.api_key)", INI_STR("tideways.api_key"));
	php_info_print_table_row(2, "Default Sample-Rate (tideways.sample_rate)", INI_STR("tideways.sample_rate"));
	php_info_print_table_row(2, "Framework Detection (tideways.framework)", INI_STR("tideways.framework"));
	php_info_print_table_row(2, "Automatically Start (tideways.auto_start)", INI_INT("tideways.auto_start") ? "Yes": "No");
	php_info_print_table_row(2, "Tideways Collect Mode (tideways.collect)", INI_STR("tideways.collect"));
	php_info_print_table_row(2, "Tideways Monitoring Mode (tideways.monitor)", INI_STR("tideways.monitor"));
	php_info_print_table_row(2, "Allowed Distributed Tracing Hosts (tideways.distributed_tracing_hosts)", INI_STR("tideways.distributed_tracing_hosts"));
	php_info_print_table_row(2, "Load PHP Library (tideways.auto_prepend_library)", INI_INT("tideways.auto_prepend_library") ? "Yes": "No");

	extension_dir  = INI_STR("extension_dir");
	profiler_file_len = strlen(extension_dir) + strlen("Tideways.php") + 2;
	profiler_file = emalloc(profiler_file_len);
	snprintf(profiler_file, profiler_file_len, "%s/%s", extension_dir, "Tideways.php");

	if (VCWD_ACCESS(profiler_file, F_OK) == 0) {
		php_info_print_table_row(2, "Tideways.php found", "Yes");
	} else {
		php_info_print_table_row(2, "Tideways.php found", "No");
	}

	efree(profiler_file);

	php_info_print_table_end();
}


/**
 * ***************************************************
 * COMMON HELPER FUNCTION DEFINITIONS AND LOCAL MACROS
 * ***************************************************
 */

inline static void hp_register_constants(INIT_FUNC_ARGS)
{
	REGISTER_LONG_CONSTANT("TIDEWAYS_FLAGS_CPU", TIDEWAYS_FLAGS_CPU, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("TIDEWAYS_FLAGS_MEMORY", TIDEWAYS_FLAGS_MEMORY, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("TIDEWAYS_FLAGS_NO_BUILTINS", TIDEWAYS_FLAGS_NO_BUILTINS, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("TIDEWAYS_FLAGS_NO_USERLAND", TIDEWAYS_FLAGS_NO_USERLAND, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("TIDEWAYS_FLAGS_NO_COMPILE", TIDEWAYS_FLAGS_NO_COMPILE, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("TIDEWAYS_FLAGS_NO_SPANS", TIDEWAYS_FLAGS_NO_SPANS, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("TIDEWAYS_FLAGS_NO_HIERACHICAL", TIDEWAYS_FLAGS_NO_HIERACHICAL, CONST_CS | CONST_PERSISTENT);
}

/**
 * A hash function to calculate a 8-bit hash code for a function name.
 * This is based on a small modification to 'zend_inline_hash_func' by summing
 * up all bytes of the ulong returned by 'zend_inline_hash_func'.
 *
 * @param str, char *, string to be calculated hash code for.
 *
 * @author cjiang
 */
static inline uint8 hp_inline_hash(char * arKey)
{
	size_t nKeyLength = strlen(arKey);
	register uint8 hash = 0;

	/* variant with the hash unrolled eight times */
	for (; nKeyLength >= 8; nKeyLength -= 8) {
		hash = ((hash << 5) + hash) + *arKey++;
		hash = ((hash << 5) + hash) + *arKey++;
		hash = ((hash << 5) + hash) + *arKey++;
		hash = ((hash << 5) + hash) + *arKey++;
		hash = ((hash << 5) + hash) + *arKey++;
		hash = ((hash << 5) + hash) + *arKey++;
		hash = ((hash << 5) + hash) + *arKey++;
		hash = ((hash << 5) + hash) + *arKey++;
	}
	switch (nKeyLength) {
		case 7: hash = ((hash << 5) + hash) + *arKey++; /* fallthrough... */
		case 6: hash = ((hash << 5) + hash) + *arKey++; /* fallthrough... */
		case 5: hash = ((hash << 5) + hash) + *arKey++; /* fallthrough... */
		case 4: hash = ((hash << 5) + hash) + *arKey++; /* fallthrough... */
		case 3: hash = ((hash << 5) + hash) + *arKey++; /* fallthrough... */
		case 2: hash = ((hash << 5) + hash) + *arKey++; /* fallthrough... */
		case 1: hash = ((hash << 5) + hash) + *arKey++; break;
		case 0: break;
EMPTY_SWITCH_DEFAULT_CASE()
	}
	return hash;
}

/**
 * Parse the list of ignored functions from the zval argument.
 *
 * @author mpal
 */
static void hp_parse_options_from_arg(zval *args)
{
	hp_clean_profiler_options_state();

	if (args == NULL) {
		return;
	}

	zval  *zresult = NULL;

	zresult = hp_zval_at_key("ignored_functions", args);

	if (zresult == NULL) {
		zresult = hp_zval_at_key("functions", args);
		if (zresult != NULL) {
			hp_globals.filtered_type = 2;
		}
	} else {
		hp_globals.filtered_type = 1;
	}

	hp_globals.filtered_functions = hp_function_map_create(hp_strings_in_zval(zresult));

	zresult = hp_zval_at_key("transaction_function", args);

	if (zresult != NULL) {
		hp_globals.transaction_function = hp_zval_to_string(zresult);
	}

	zresult = hp_zval_at_key("exception_function", args);

	if (zresult != NULL) {
		hp_globals.exception_function = hp_zval_to_string(zresult);
	}
}

static void hp_exception_function_clear() {
	if (hp_globals.exception_function != NULL) {
		hp_string_clean(hp_globals.exception_function);
		efree(hp_globals.exception_function);
		hp_globals.exception_function = NULL;
	}

	if (hp_globals.exception != NULL) {
		zval_ptr_dtor(&hp_globals.exception);
	}
}

static void hp_transaction_function_clear() {
	if (hp_globals.transaction_function) {
		hp_string_clean(hp_globals.transaction_function);
		efree(hp_globals.transaction_function);
		hp_globals.transaction_function = NULL;
	}
}

static inline hp_function_map *hp_function_map_create(char **names)
{
	if (names == NULL) {
		return NULL;
	}

	hp_function_map *map;

	map = emalloc(sizeof(hp_function_map));
	map->names = names;

	memset(map->filter, 0, TIDEWAYS_FILTERED_FUNCTION_SIZE);

	int i = 0;
	for(; names[i] != NULL; i++) {
		char *str  = names[i];
		uint8 hash = hp_inline_hash(str);
		int   idx  = INDEX_2_BYTE(hash);
		map->filter[idx] |= INDEX_2_BIT(hash);
	}

	return map;
}

static inline void hp_function_map_clear(hp_function_map *map) {
	if (map == NULL) {
		return;
	}

	hp_array_del(map->names);
	map->names = NULL;

	memset(map->filter, 0, TIDEWAYS_FILTERED_FUNCTION_SIZE);
	efree(map);
}

static inline int hp_function_map_exists(hp_function_map *map, uint8 hash_code, char *curr_func)
{
	if (hp_function_map_filter_collision(map, hash_code)) {
		int i = 0;
		for (; map->names[i] != NULL; i++) {
			char *name = map->names[i];
			if (strcmp(curr_func, name) == 0) {
				return 1;
			}
		}
	}

	return 0;
}


static inline int hp_function_map_filter_collision(hp_function_map *map, uint8 hash)
{
	uint8 mask = INDEX_2_BIT(hash);
	return map->filter[INDEX_2_BYTE(hash)] & mask;
}

void hp_init_trace_callbacks(TSRMLS_D)
{
	tw_trace_callback cb;

	if ((hp_globals.tideways_flags & TIDEWAYS_FLAGS_NO_SPANS) > 0) {
		return;
	}

	hp_globals.trace_callbacks = NULL;
	hp_globals.trace_watch_callbacks = NULL;
	hp_globals.span_cache = NULL;

	ALLOC_HASHTABLE(hp_globals.trace_callbacks);
	zend_hash_init(hp_globals.trace_callbacks, 255, NULL, NULL, 0);

	ALLOC_HASHTABLE(hp_globals.span_cache);
	zend_hash_init(hp_globals.span_cache, 255, NULL, NULL, 0);

	cb = tw_trace_callback_file_get_contents;
	register_trace_callback("file_get_contents", cb);

	cb = tw_trace_callback_php_call;
	register_trace_callback("session_start", cb);
	// Symfony
	register_trace_callback("Symfony\\Component\\HttpKernel\\Kernel::boot", cb);
	register_trace_callback("Symfony\\Component\\EventDispatcher\\ContainerAwareEventDispatcher::lazyLoad", cb);
	// Wordpress
	register_trace_callback("get_sidebar", cb);
	register_trace_callback("get_header", cb);
	register_trace_callback("get_footer", cb);
	register_trace_callback("load_textdomain", cb);
	register_trace_callback("setup_theme", cb);
	// Doctrine
	register_trace_callback("Doctrine\\ORM\\EntityManager::flush", cb);
	register_trace_callback("Doctrine\\ODM\\CouchDB\\DocumentManager::flush", cb);
	// Magento
	register_trace_callback("Mage_Core_Model_App::_initModules", cb);
	register_trace_callback("Mage_Core_Model_Config::loadModules", cb);
	register_trace_callback("Mage_Core_Model_Config::loadDb", cb);
	// Smarty&Twig Compiler
	register_trace_callback("Smarty_Internal_TemplateCompilerBase::compileTemplate", cb);
	register_trace_callback("Twig_Environment::compileSource", cb);
	// Shopware Assets (very special, do we really need it?)
	register_trace_callback("JSMin::minify", cb);
	register_trace_callback("Less_Parser::getCss", cb);
	// Laravel (4+5)
	register_trace_callback("Illuminate\\Foundation\\Application::boot", cb);
	register_trace_callback("Illuminate\\Foundation\\Application::dispatch", cb);
	// Silex
	register_trace_callback("Silex\\Application::mount", cb);

	cb = tw_trace_callback_doctrine_persister;
	register_trace_callback("Doctrine\\ORM\\Persisters\\BasicEntityPersister::load", cb);
	register_trace_callback("Doctrine\\ORM\\Persisters\\BasicEntityPersister::loadAll", cb);
	register_trace_callback("Doctrine\\ORM\\Persisters\\Entity\\BasicEntityPersister::load", cb);
	register_trace_callback("Doctrine\\ORM\\Persisters\\Entity\\BasicEntityPersister::loadAll", cb);

	cb = tw_trace_callback_doctrine_query;
	register_trace_callback("Doctrine\\ORM\\AbstractQuery::execute", cb);

	cb = tw_trace_callback_doctrine_couchdb_request;
	register_trace_callback("Doctrine\\CouchDB\\HTTP\\SocketClient::request", cb);
	register_trace_callback("Doctrine\\CouchDB\\HTTP\\StreamClient::request", cb);

	cb = tw_trace_callback_curl_exec;
	register_trace_callback("curl_exec", cb);

	cb = tw_trace_callback_sql_functions;
	register_trace_callback("PDO::exec", cb);
	register_trace_callback("PDO::query", cb);
	register_trace_callback("mysql_query", cb);
	register_trace_callback("mysqli_query", cb);
	register_trace_callback("mysqli::query", cb);
	register_trace_callback("mysqli::prepare", cb);
	register_trace_callback("mysqli_prepare", cb);

	cb = tw_trace_callback_sql_commit;
	register_trace_callback("PDO::commit", cb);
	register_trace_callback("mysqli::commit", cb);
	register_trace_callback("mysqli_commit", cb);

	cb = tw_trace_callback_pdo_stmt_execute;
	register_trace_callback("PDOStatement::execute", cb);

	cb = tw_trace_callback_mysqli_stmt_execute;
	register_trace_callback("mysqli_stmt_execute", cb);
	register_trace_callback("mysqli_stmt::execute", cb);

	cb = tw_trace_callback_pgsql_query;
	register_trace_callback("pg_query", cb);
	register_trace_callback("pg_query_params", cb);

	cb = tw_trace_callback_pgsql_execute;
	register_trace_callback("pg_execute", cb);

	cb = tw_trace_callback_event_dispatchers;
	register_trace_callback("Doctrine\\Common\\EventManager::dispatchEvent", cb);
	register_trace_callback("Enlight_Event_EventManager::filter", cb);
	register_trace_callback("Enlight_Event_EventManager::notify", cb);
	register_trace_callback("Enlight_Event_EventManager::notifyUntil", cb);
	register_trace_callback("Zend\\EventManager\\EventManager::trigger", cb);
	register_trace_callback("do_action", cb);
	register_trace_callback("drupal_alter", cb);
	register_trace_callback("Mage::dispatchEvent", cb);
	register_trace_callback("Symfony\\Component\\EventDispatcher\\EventDispatcher::dispatch", cb);
	register_trace_callback("Illuminate\\Events\\Dispatcher::fire", cb);

	cb = tw_trace_callback_twig_template;
	register_trace_callback("Twig_Template::render", cb);
	register_trace_callback("Twig_Template::display", cb);

	cb = tw_trace_callback_smarty3_template;
	register_trace_callback("Smarty_Internal_TemplateBase::fetch", cb);

	cb = tw_trace_callback_fastcgi_finish_request;
	register_trace_callback("fastcgi_finish_request", cb);

	cb = tw_trace_callback_soap_client_dorequest;
	register_trace_callback("SoapClient::__doRequest", cb);

	cb = tw_trace_callback_magento_block;
	register_trace_callback("Mage_Core_Block_Abstract::toHtml", cb);

	cb = tw_trace_callback_view_engine;
	register_trace_callback("Zend_View_Abstract::render", cb);
	register_trace_callback("Illuminate\\View\\Engines\\CompilerEngine::get", cb);
	register_trace_callback("Smarty::fetch", cb);
	register_trace_callback("load_template", cb);

	cb = tw_trace_callback_zend1_dispatcher_families_tx;
	register_trace_callback("Enlight_Controller_Action::dispatch", cb);
	register_trace_callback("Mage_Core_Controller_Varien_Action::dispatch", cb);
	register_trace_callback("Zend_Controller_Action::dispatch", cb);
	register_trace_callback("Illuminate\\Routing\\Controller::callAction", cb);

	cb = tw_trace_callback_symfony_resolve_arguments_tx;
	register_trace_callback("Symfony\\Component\\HttpKernel\\Controller\\ControllerResolver::getArguments", cb);

	cb = tw_trace_callback_oxid_tx;
	register_trace_callback("oxShopControl::_process", cb);

	// Different versions of Memcache Extension have either MemcachePool or Memcache class, @todo investigate
	cb = tw_trace_callback_memcache;
	register_trace_callback("MemcachePool::get", cb);
	register_trace_callback("MemcachePool::set", cb);
	register_trace_callback("MemcachePool::delete", cb);
	register_trace_callback("MemcachePool::flush", cb);
	register_trace_callback("MemcachePool::replace", cb);
	register_trace_callback("MemcachePool::increment", cb);
	register_trace_callback("MemcachePool::decrement", cb);
	register_trace_callback("Memcache::get", cb);
	register_trace_callback("Memcache::set", cb);
	register_trace_callback("Memcache::delete", cb);
	register_trace_callback("Memcache::flush", cb);
	register_trace_callback("Memcache::replace", cb);
	register_trace_callback("Memcache::increment", cb);
	register_trace_callback("Memcache::decrement", cb);

	cb = tw_trace_callback_pheanstalk;
	register_trace_callback("Pheanstalk_Pheanstalk::put", cb);
	register_trace_callback("Pheanstalk\\Pheanstalk::put", cb);

	cb = tw_trace_callback_phpampqlib;
	register_trace_callback("PhpAmqpLib\\Channel\\AMQPChannel::basic_publish", cb);

	cb = tw_trace_callback_mongo_collection;
	register_trace_callback("MongoCollection::find", cb);
	register_trace_callback("MongoCollection::findOne", cb);
	register_trace_callback("MongoCollection::findAndModify", cb);
	register_trace_callback("MongoCollection::insert", cb);
	register_trace_callback("MongoCollection::remove", cb);
	register_trace_callback("MongoCollection::save", cb);
	register_trace_callback("MongoCollection::update", cb);
	register_trace_callback("MongoCollection::group", cb);
	register_trace_callback("MongoCollection::distinct", cb);
	register_trace_callback("MongoCollection::batchInsert", cb);
	register_trace_callback("MongoCollection::aggregate", cb);
	register_trace_callback("MongoCollection::aggregateCursor", cb);

	cb = tw_trace_callback_mongo_cursor_next;
	register_trace_callback("MongoCursor::next", cb);
	register_trace_callback("MongoCursor::hasNext", cb);
	register_trace_callback("MongoCursor::getNext", cb);
	register_trace_callback("MongoCommandCursor::next", cb);
	register_trace_callback("MongoCommandCursor::hasNext", cb);
	register_trace_callback("MongoCommandCursor::getNext", cb);

	cb = tw_trace_callback_mongo_cursor_io;
	register_trace_callback("MongoCursor::rewind", cb);
	register_trace_callback("MongoCursor::doQuery", cb);
	register_trace_callback("MongoCursor::count", cb);

	cb = tw_trace_callback_predis_call;
	register_trace_callback("Predis\\Client::__call", cb);

	hp_globals.gc_runs = GC_G(gc_runs);
	hp_globals.gc_collected = GC_G(collected);
	hp_globals.compile_count = 0;
	hp_globals.compile_wt = 0;
}


/**
 * Initialize profiler state
 *
 * @author kannan, veeve
 */
void hp_init_profiler_state(TSRMLS_D)
{
	/* Setup globals */
	if (!hp_globals.ever_enabled) {
		hp_globals.ever_enabled  = 1;
		hp_globals.entries = NULL;
	}

	/* Init stats_count */
	if (hp_globals.stats_count) {
		zval_ptr_dtor(&hp_globals.stats_count);
	}
	MAKE_STD_ZVAL(hp_globals.stats_count);
	array_init(hp_globals.stats_count);

	if (hp_globals.spans) {
		zval_ptr_dtor(&hp_globals.spans);
	}
	MAKE_STD_ZVAL(hp_globals.spans);
	array_init(hp_globals.spans);

	/* Set up filter of functions which may be ignored during profiling */
	hp_transaction_name_clear();

	hp_init_trace_callbacks(TSRMLS_C);
}

/**
 * Cleanup profiler state
 *
 * @author kannan, veeve
 */
void hp_clean_profiler_state(TSRMLS_D)
{
	/* Clear globals */
	if (hp_globals.stats_count) {
		zval_ptr_dtor(&hp_globals.stats_count);
		hp_globals.stats_count = NULL;
	}
	if (hp_globals.spans) {
		zval_ptr_dtor(&hp_globals.spans);
		hp_globals.spans = NULL;
	}

	hp_globals.entries = NULL;
	hp_globals.ever_enabled = 0;

	hp_clean_profiler_options_state();

	hp_function_map_clear(hp_globals.filtered_functions);
	hp_globals.filtered_functions = NULL;
}

static void hp_transaction_name_clear()
{
	if (hp_globals.transaction_name) {
		hp_string_clean(hp_globals.transaction_name);
		efree(hp_globals.transaction_name);
		hp_globals.transaction_name = NULL;
	}
}

static void hp_clean_profiler_options_state()
{
	hp_function_map_clear(hp_globals.filtered_functions);
	hp_globals.filtered_functions = NULL;

	hp_exception_function_clear();
	hp_transaction_function_clear();
	hp_transaction_name_clear();

	if (hp_globals.trace_callbacks) {
		zend_hash_destroy(hp_globals.trace_callbacks);
		FREE_HASHTABLE(hp_globals.trace_callbacks);
		hp_globals.trace_callbacks = NULL;
	}

	if (hp_globals.trace_watch_callbacks) {
		zend_hash_destroy(hp_globals.trace_watch_callbacks);
		FREE_HASHTABLE(hp_globals.trace_watch_callbacks);
		hp_globals.trace_watch_callbacks = NULL;
	}

	if (hp_globals.span_cache) {
		zend_hash_destroy(hp_globals.span_cache);
		FREE_HASHTABLE(hp_globals.span_cache);
		hp_globals.span_cache = NULL;
	}
}

/*
 * Start profiling - called just before calling the actual function
 * NOTE:  PLEASE MAKE SURE TSRMLS_CC IS AVAILABLE IN THE CONTEXT
 *        OF THE FUNCTION WHERE THIS MACRO IS CALLED.
 *        TSRMLS_CC CAN BE MADE AVAILABLE VIA TSRMLS_DC IN THE
 *        CALLING FUNCTION OR BY CALLING TSRMLS_FETCH()
 *        TSRMLS_FETCH() IS RELATIVELY EXPENSIVE.
 */
#define BEGIN_PROFILING(entries, symbol, profile_curr, execute_data)			\
	do {																		\
		/* Use a hash code to filter most of the string comparisons. */			\
		uint8 hash_code  = hp_inline_hash(symbol);								\
		profile_curr = !hp_filter_entry(hash_code, symbol);						\
		if (profile_curr) {														\
			hp_entry_t *cur_entry = hp_fast_alloc_hprof_entry();				\
			(cur_entry)->hash_code = hash_code;									\
			(cur_entry)->name_hprof = symbol;									\
			(cur_entry)->prev_hprof = (*(entries));								\
			(cur_entry)->span_id = -1;											\
			hp_mode_hier_beginfn_cb((entries), (cur_entry), execute_data TSRMLS_CC);			\
			/* Update entries linked list */									\
			(*(entries)) = (cur_entry);											\
		}																		\
	} while (0)

/*
 * Stop profiling - called just after calling the actual function
 * NOTE:  PLEASE MAKE SURE TSRMLS_CC IS AVAILABLE IN THE CONTEXT
 *        OF THE FUNCTION WHERE THIS MACRO IS CALLED.
 *        TSRMLS_CC CAN BE MADE AVAILABLE VIA TSRMLS_DC IN THE
 *        CALLING FUNCTION OR BY CALLING TSRMLS_FETCH()
 *        TSRMLS_FETCH() IS RELATIVELY EXPENSIVE.
 */
#define END_PROFILING(entries, profile_curr, data)							\
	do {																	\
		if (profile_curr) {													\
			hp_entry_t *cur_entry;											\
			hp_mode_hier_endfn_cb((entries), data TSRMLS_CC);				\
			cur_entry = (*(entries));										\
			/* Free top entry and update entries linked list */				\
			(*(entries)) = (*(entries))->prev_hprof;						\
			hp_fast_free_hprof_entry(cur_entry);							\
		}																	\
	} while (0)

/**
 * Returns formatted function name
 *
 * @param  entry        hp_entry
 * @param  result_buf   ptr to result buf
 * @param  result_len   max size of result buf
 * @return total size of the function name returned in result_buf
 * @author veeve
 */
size_t hp_get_entry_name(hp_entry_t  *entry, char *result_buf, size_t result_len)
{
	/* Validate result_len */
	if (result_len <= 1) {
		/* Insufficient result_bug. Bail! */
		return 0;
	}

	/* Add '@recurse_level' if required */
	/* NOTE:  Dont use snprintf's return val as it is compiler dependent */
	if (entry->rlvl_hprof) {
		snprintf(
			result_buf,
			result_len,
			"%s@%d",
			entry->name_hprof,
			entry->rlvl_hprof
		);
	} else {
		strncat(
			result_buf,
			entry->name_hprof,
			result_len
		);
	}

	/* Force null-termination at MAX */
	result_buf[result_len - 1] = 0;

	return strlen(result_buf);
}

/**
 * Check if this entry should be filtered (positive or negative), first with a
 * conservative Bloomish filter then with an exact check against the function
 * names.
 *
 * @author mpal
 */
static inline int hp_filter_entry(uint8 hash_code, char *curr_func)
{
	int exists;

	/* First check if ignoring functions is enabled */
	if (hp_globals.filtered_functions == NULL || hp_globals.filtered_type == 0) {
		return 0;
	}

	exists = hp_function_map_exists(hp_globals.filtered_functions, hash_code, curr_func);

	if (hp_globals.filtered_type == 2) {
		// always include main() in profiling result.
		return (strcmp(curr_func, ROOT_SYMBOL) == 0)
			? 0
			: abs(1 - exists);
	}

	return exists;
}

/**
 * Build a caller qualified name for a callee.
 *
 * For example, if A() is caller for B(), then it returns "A==>B".
 * Recursive invokations are denoted with @<n> where n is the recursion
 * depth.
 *
 * For example, "foo==>foo@1", and "foo@2==>foo@3" are examples of direct
 * recursion. And  "bar==>foo@1" is an example of an indirect recursive
 * call to foo (implying the foo() is on the call stack some levels
 * above).
 *
 * @author kannan, veeve
 */
size_t hp_get_function_stack(hp_entry_t *entry, int level, char *result_buf, size_t result_len)
{
	size_t         len = 0;

	if (!entry->prev_hprof || (level <= 1)) {
		return hp_get_entry_name(entry, result_buf, result_len);
	}

	len = hp_get_function_stack(entry->prev_hprof, level - 1, result_buf, result_len);

	/* Append the delimiter */
# define    HP_STACK_DELIM        "==>"
# define    HP_STACK_DELIM_LEN    (sizeof(HP_STACK_DELIM) - 1)

	if (result_len < (len + HP_STACK_DELIM_LEN)) {
		return len;
	}

	if (len) {
		strncat(result_buf + len, HP_STACK_DELIM, result_len - len);
		len += HP_STACK_DELIM_LEN;
	}

# undef     HP_STACK_DELIM_LEN
# undef     HP_STACK_DELIM

	return len + hp_get_entry_name(entry, result_buf + len, result_len - len);
}

/**
 * Takes an input of the form /a/b/c/d/foo.php and returns
 * a pointer to one-level directory and basefile name
 * (d/foo.php) in the same string.
 */
static char *hp_get_base_filename(char *filename)
{
	char *ptr;
	int   found = 0;

	if (!filename)
		return "";

	/* reverse search for "/" and return a ptr to the next char */
	for (ptr = filename + strlen(filename) - 1; ptr >= filename; ptr--) {
		if (*ptr == '/') {
			found++;
		}
		if (found == 2) {
			return ptr + 1;
		}
	}

	/* no "/" char found, so return the whole string */
	return filename;
}

static char *hp_get_file_summary(char *filename, int filename_len TSRMLS_DC)
{
	php_url *url;
	char *ret;
	int len;

	len = TIDEWAYS_MAX_ARGUMENT_LEN;
	ret = emalloc(len);
	snprintf(ret, len, "");

	url = php_url_parse_ex(filename, filename_len);

	if (url->scheme) {
		snprintf(ret, len, "%s%s://", ret, url->scheme);
	} else {
		php_url_free(url);
		return ret;
	}

	if (url->host) {
		snprintf(ret, len, "%s%s", ret, url->host);
	}

	if (url->port) {
		snprintf(ret, len, "%s:%d", ret, url->port);
	}

	if (url->path) {
		snprintf(ret, len, "%s%s", ret, url->path);
	}

	php_url_free(url);

	return ret;
}

static inline void **hp_get_execute_arguments(zend_execute_data *data)
{
	void **p;

	p = data->function_state.arguments;

#if PHP_VERSION_ID >= 50500
	/*
	 * With PHP 5.5 zend_execute cannot be overwritten by extensions anymore.
	 * instead zend_execute_ex has to be used. That however does not have
	 * function_state.arguments populated for non-internal functions.
	 * As per UPGRADING.INTERNALS we are accessing prev_execute_data which
	 * has this information (for whatever reasons).
	 */
	if (p == NULL) {
		p = (*data).prev_execute_data->function_state.arguments;
	}
#endif

	return p;
}

static char *hp_concat_char(const char *s1, size_t len1, const char *s2, size_t len2, const char *seperator, size_t sep_len)
{
    char *result = emalloc(len1+len2+sep_len+1);

    strcpy(result, s1);
	strcat(result, seperator);
    strcat(result, s2);

    return result;
}

static void hp_detect_exception(char *func_name, zend_execute_data *data TSRMLS_DC)
{
	void **p = hp_get_execute_arguments(data);
	int arg_count = (int)(zend_uintptr_t) *p;
	zval *argument_element;
	int i;
	zend_class_entry *default_ce, *exception_ce;

	default_ce = zend_exception_get_default(TSRMLS_C);

	for (i=0; i < arg_count; i++) {
		argument_element = *(p-(arg_count-i));

		if (Z_TYPE_P(argument_element) == IS_OBJECT) {
			exception_ce = zend_get_class_entry(argument_element TSRMLS_CC);

			if (instanceof_function(exception_ce, default_ce TSRMLS_CC) == 1) {
				Z_ADDREF_P(argument_element);
				hp_globals.exception = argument_element;
				return;
			}
		}
	}
}

static void hp_detect_transaction_name(char *ret, zend_execute_data *data TSRMLS_DC)
{
	if (!hp_globals.transaction_function ||
		hp_globals.transaction_name ||
		strcmp(ret, hp_globals.transaction_function->value) != 0) {
		return;
	}

	void **p = hp_get_execute_arguments(data);
	int arg_count = (int)(zend_uintptr_t) *p;
	zval *argument_element;

	if (strcmp(ret, "Zend_Controller_Action::dispatch") == 0 ||
			   strcmp(ret, "Enlight_Controller_Action::dispatch") == 0 ||
			   strcmp(ret, "Mage_Core_Controller_Varien_Action::dispatch") == 0 ||
			   strcmp(ret, "Illuminate\\Routing\\Controller::callAction") == 0) {
		zval *obj = data->object;
		argument_element = *(p-arg_count);
		const char *class_name;
		zend_uint class_name_len;
		const char *free_class_name = NULL;

		if (!zend_get_object_classname(obj, &class_name, &class_name_len TSRMLS_CC)) {
			free_class_name = class_name;
		}

		if (Z_TYPE_P(argument_element) == IS_STRING) {
			int len = class_name_len + Z_STRLEN_P(argument_element) + 3;
			char *ret = NULL;
			ret = (char*)emalloc(len);
			snprintf(ret, len, "%s::%s", class_name, Z_STRVAL_P(argument_element));

			hp_globals.transaction_name = hp_create_string(ret, len);
			efree(ret);
		}

		if (free_class_name) {
			efree((char*)free_class_name);
		}
	} else {
		argument_element = *(p-arg_count);

		if (Z_TYPE_P(argument_element) == IS_STRING) {
			hp_globals.transaction_name = hp_zval_to_string(argument_element);
		}
	}

	hp_transaction_function_clear();
}

/**
 * Get the name of the current function. The name is qualified with
 * the class name if the function is in a class.
 *
 * @author kannan, hzhao
 */
static char *hp_get_function_name(zend_execute_data *data TSRMLS_DC)
{
	const char        *func = NULL;
	const char        *cls = NULL;
	char              *ret = NULL;
	int                len;
	zend_function      *curr_func;

	if (!data) {
		return NULL;
	}

	curr_func = data->function_state.function;
	func = curr_func->common.function_name;

	if (!func) {
		// This branch includes execution of eval and include/require(_once) calls
		// We assume it is not 1999 anymore and not much PHP code runs in the
		// body of a file and if it is, we are ok with adding it to the caller's wt.
		return NULL;
	}

	/* previously, the order of the tests in the "if" below was
	 * flipped, leading to incorrect function names in profiler
	 * reports. When a method in a super-type is invoked the
	 * profiler should qualify the function name with the super-type
	 * class name (not the class name based on the run-time type
	 * of the object.
	 */
	if (curr_func->common.scope) {
		cls = curr_func->common.scope->name;
	} else if (data->object) {
		cls = Z_OBJCE(*data->object)->name;
	}

	if (cls) {
		char* sep = "::";
		ret = hp_concat_char(cls, strlen(cls), func, strlen(func), sep, 2);
	} else {
		ret = estrdup(func);
	}

	return ret;
}

/**
 * Free any items in the free list.
 */
static void hp_free_the_free_list()
{
	hp_entry_t *p = hp_globals.entry_free_list;
	hp_entry_t *cur;

	while (p) {
		cur = p;
		p = p->prev_hprof;
		free(cur);
	}
}

/**
 * Fast allocate a hp_entry_t structure. Picks one from the
 * free list if available, else does an actual allocate.
 *
 * Doesn't bother initializing allocated memory.
 *
 * @author kannan
 */
static hp_entry_t *hp_fast_alloc_hprof_entry()
{
	hp_entry_t *p;

	p = hp_globals.entry_free_list;

	if (p) {
		hp_globals.entry_free_list = p->prev_hprof;
		return p;
	} else {
		return (hp_entry_t *)malloc(sizeof(hp_entry_t));
	}
}

/**
 * Fast free a hp_entry_t structure. Simply returns back
 * the hp_entry_t to a free list and doesn't actually
 * perform the free.
 *
 * @author kannan
 */
static void hp_fast_free_hprof_entry(hp_entry_t *p)
{
	/* we use/overload the prev_hprof field in the structure to link entries in
	 * the free list. */
	p->prev_hprof = hp_globals.entry_free_list;
	hp_globals.entry_free_list = p;
}

/**
 * Increment the count of the given stat with the given count
 * If the stat was not set before, inits the stat to the given count
 *
 * @param  zval *counts   Zend hash table pointer
 * @param  char *name     Name of the stat
 * @param  long  count    Value of the stat to incr by
 * @return void
 * @author kannan
 */
void hp_inc_count(zval *counts, char *name, long count TSRMLS_DC)
{
	HashTable *ht;
	void *data;

	if (!counts) return;
	ht = HASH_OF(counts);
	if (!ht) return;

	if (zend_hash_find(ht, name, strlen(name) + 1, &data) == SUCCESS) {
		ZVAL_LONG(*(zval**)data, Z_LVAL_PP((zval**)data) + count);
	} else {
		add_assoc_long(counts, name, count);
	}
}

/**
 * Looksup the hash table for the given symbol
 * Initializes a new array() if symbol is not present
 *
 * @author kannan, veeve
 */
zval * hp_hash_lookup(zval *hash, char *symbol  TSRMLS_DC)
{
	HashTable   *ht;
	void        *data;
	zval        *counts = (zval *) 0;

	/* Bail if something is goofy */
	if (!hash || !(ht = HASH_OF(hash))) {
		return (zval *) 0;
	}

	/* Lookup our hash table */
	if (zend_hash_find(ht, symbol, strlen(symbol) + 1, &data) == SUCCESS) {
		/* Symbol already exists */
		counts = *(zval **) data;
	}
	else {
		/* Add symbol to hash table */
		MAKE_STD_ZVAL(counts);
		array_init(counts);
		add_assoc_zval(hash, symbol, counts);
	}

	return counts;
}

/**
 * Truncates the given timeval to the nearest slot begin, where
 * the slot size is determined by intr
 *
 * @param  tv       Input timeval to be truncated in place
 * @param  intr     Time interval in microsecs - slot width
 * @return void
 * @author veeve
 */
void hp_trunc_time(struct timeval *tv, uint64 intr)
{
	uint64 time_in_micro;

	/* Convert to microsecs and trunc that first */
	time_in_micro = (tv->tv_sec * 1000000) + tv->tv_usec;
	time_in_micro /= intr;
	time_in_micro *= intr;

	/* Update tv */
	tv->tv_sec  = (time_in_micro / 1000000);
	tv->tv_usec = (time_in_micro % 1000000);
}

/**
 * ***********************
 * High precision timer related functions.
 * ***********************
 */

/**
 * Get the current wallclock timer
 *
 * @return 64 bit unsigned integer
 * @author cjiang
 */
static uint64 cycle_timer() {
#ifdef __APPLE__
	return mach_absolute_time();
#else
	struct timespec s;
	clock_gettime(CLOCK_MONOTONIC, &s);

	return s.tv_sec * 1000000 + s.tv_nsec / 1000;
#endif
}

/**
 * Get the current real CPU clock timer
 */
static uint64 cpu_timer() {
#if defined(CLOCK_PROCESS_CPUTIME_ID)
	struct timespec s;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &s);

	return s.tv_sec * 1000000 + s.tv_nsec / 1000;
#else
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);

	return ru.ru_utime.tv_sec * 1000000 + ru.ru_utime.tv_usec +
		ru.ru_stime.tv_sec * 1000000 + ru.ru_stime.tv_usec;
#endif
}

/**
 * Get time delta in microseconds.
 */
static long get_us_interval(struct timeval *start, struct timeval *end)
{
	return (((end->tv_sec - start->tv_sec) * 1000000)
			+ (end->tv_usec - start->tv_usec));
}

/**
 * Convert from TSC counter values to equivalent microseconds.
 *
 * @param uint64 count, TSC count value
 * @return 64 bit unsigned integer
 *
 * @author cjiang
 */
static inline double get_us_from_tsc(uint64 count)
{
	return count / hp_globals.timebase_factor;
}

/**
 * Get the timebase factor necessary to divide by in cycle_timer()
 */
static double get_timebase_factor()
{
#ifdef __APPLE__
	mach_timebase_info_data_t sTimebaseInfo;
	(void) mach_timebase_info(&sTimebaseInfo);

	return (sTimebaseInfo.numer / sTimebaseInfo.denom) * 1000;
#else
	return 1.0;
#endif
}

/**
 * TIDEWAYS_MODE_HIERARCHICAL's begin function callback
 *
 * @author kannan
 */
void hp_mode_hier_beginfn_cb(hp_entry_t **entries, hp_entry_t *current, zend_execute_data *data TSRMLS_DC)
{
	hp_entry_t   *p;
	tw_trace_callback *callback;
	int    recurse_level = 0;

	if ((hp_globals.tideways_flags & TIDEWAYS_FLAGS_NO_HIERACHICAL) == 0) {
		if (hp_globals.func_hash_counters[current->hash_code] > 0) {
			/* Find this symbols recurse level */
			for(p = (*entries); p; p = p->prev_hprof) {
				if (!strcmp(current->name_hprof, p->name_hprof)) {
					recurse_level = (p->rlvl_hprof) + 1;
					break;
				}
			}
		}
		hp_globals.func_hash_counters[current->hash_code]++;

		/* Init current function's recurse level */
		current->rlvl_hprof = recurse_level;
	}

	/* Get start tsc counter */
	current->tsc_start = cycle_timer();

	if ((hp_globals.tideways_flags & TIDEWAYS_FLAGS_NO_SPANS) == 0) {
		if (data != NULL && zend_hash_find(hp_globals.trace_callbacks, current->name_hprof, strlen(current->name_hprof)+1, (void **)&callback) == SUCCESS) {
			void **args =  hp_get_execute_arguments(data);
			int arg_count = (int)(zend_uintptr_t) *args;
			zval *obj = data->object;

			current->span_id = (*callback)(current->name_hprof, args, arg_count, obj TSRMLS_CC);
		}
	}

	/* Get CPU usage */
	if (hp_globals.tideways_flags & TIDEWAYS_FLAGS_CPU) {
		current->cpu_start = cpu_timer();
	}

	/* Get memory usage */
	if (hp_globals.tideways_flags & TIDEWAYS_FLAGS_MEMORY) {
		current->mu_start_hprof  = zend_memory_usage(0 TSRMLS_CC);
		current->pmu_start_hprof = zend_memory_peak_usage(0 TSRMLS_CC);
	}
}

/**
 * **********************************
 * TIDEWAYS END FUNCTION CALLBACKS
 * **********************************
 */

/**
 * TIDEWAYS_MODE_HIERARCHICAL's end function callback
 *
 * @author kannan
 */
void hp_mode_hier_endfn_cb(hp_entry_t **entries, zend_execute_data *data TSRMLS_DC)
{
	hp_entry_t      *top = (*entries);
	zval            *counts;
	char             symbol[SCRATCH_BUF_LEN] = "";
	long int         mu_end;
	long int         pmu_end;
	uint64   tsc_end;
	double   wt, cpu;
	tw_trace_callback *callback;

	/* Get end tsc counter */
	tsc_end = cycle_timer();
	wt = get_us_from_tsc(tsc_end - top->tsc_start);

	if (hp_globals.tideways_flags & TIDEWAYS_FLAGS_CPU) {
		cpu = get_us_from_tsc(cpu_timer() - top->cpu_start);
	}

	if ((hp_globals.tideways_flags & TIDEWAYS_FLAGS_NO_SPANS) == 0 && top->span_id >= 0) {
		double start = get_us_from_tsc(top->tsc_start - hp_globals.start_time);
		double end = get_us_from_tsc(tsc_end - hp_globals.start_time);
		tw_span_record_duration(top->span_id, start, end);
	}

	if ((hp_globals.tideways_flags & TIDEWAYS_FLAGS_NO_HIERACHICAL) > 0) {
		return;
	}

	/* Get the stat array */
	hp_get_function_stack(top, 2, symbol, sizeof(symbol));

	/* Get the stat array */
	if (!(counts = hp_hash_lookup(hp_globals.stats_count, symbol TSRMLS_CC))) {
		return;
	}

	/* Bump stats in the counts hashtable */
	hp_inc_count(counts, "ct", 1  TSRMLS_CC);
	hp_inc_count(counts, "wt", wt TSRMLS_CC);

	if (hp_globals.tideways_flags & TIDEWAYS_FLAGS_CPU) {
		/* Bump CPU stats in the counts hashtable */
		hp_inc_count(counts, "cpu", cpu TSRMLS_CC);
	}

	if (hp_globals.tideways_flags & TIDEWAYS_FLAGS_MEMORY) {
		/* Get Memory usage */
		mu_end  = zend_memory_usage(0 TSRMLS_CC);
		pmu_end = zend_memory_peak_usage(0 TSRMLS_CC);

		/* Bump Memory stats in the counts hashtable */
		hp_inc_count(counts, "mu",  mu_end - top->mu_start_hprof    TSRMLS_CC);
		hp_inc_count(counts, "pmu", pmu_end - top->pmu_start_hprof  TSRMLS_CC);
	}

	hp_globals.func_hash_counters[top->hash_code]--;
}


/**
 * ***************************
 * PHP EXECUTE/COMPILE PROXIES
 * ***************************
 */

/**
 * For transaction name detection in layer mode we only need a very simple user function overwrite.
 * Layer mode skips profiling userland functions, so we can simplify here.
 */
#if PHP_VERSION_ID < 50500
ZEND_DLEXPORT void hp_detect_tx_execute (zend_op_array *ops TSRMLS_DC) {
	zend_execute_data *execute_data = EG(current_execute_data);
	zend_execute_data *real_execute_data = execute_data;
#else
ZEND_DLEXPORT void hp_detect_tx_execute_ex (zend_execute_data *execute_data TSRMLS_DC) {
	zend_op_array *ops = execute_data->op_array;
	zend_execute_data    *real_execute_data = execute_data->prev_execute_data;
#endif
	char          *func = NULL;

	func = hp_get_function_name(real_execute_data TSRMLS_CC);
	if (func) {
		hp_detect_transaction_name(func, real_execute_data TSRMLS_CC);

		if (hp_globals.exception_function != NULL && strcmp(func, hp_globals.exception_function->value) == 0) {
			hp_detect_exception(func, real_execute_data TSRMLS_CC);
		}

		efree(func);
	}

#if PHP_VERSION_ID < 50500
	_zend_execute(ops TSRMLS_CC);
#else
	_zend_execute_ex(execute_data TSRMLS_CC);
#endif
}

/**
 * Tideways enable replaced the zend_execute function with this
 * new execute function. We can do whatever profiling we need to
 * before and after calling the actual zend_execute().
 *
 * @author hzhao, kannan
 */
#if PHP_VERSION_ID < 50500
ZEND_DLEXPORT void hp_execute (zend_op_array *ops TSRMLS_DC) {
	zend_execute_data *execute_data = EG(current_execute_data);
	zend_execute_data *real_execute_data = execute_data;
#else
ZEND_DLEXPORT void hp_execute_ex (zend_execute_data *execute_data TSRMLS_DC) {
	zend_op_array *ops = execute_data->op_array;
	zend_execute_data    *real_execute_data = execute_data->prev_execute_data;
#endif
	char          *func = NULL;
	int hp_profile_flag = 1;

	func = hp_get_function_name(real_execute_data TSRMLS_CC);
	if (!func) {
#if PHP_VERSION_ID < 50500
		_zend_execute(ops TSRMLS_CC);
#else
		_zend_execute_ex(execute_data TSRMLS_CC);
#endif
		return;
	}

	hp_detect_transaction_name(func, real_execute_data TSRMLS_CC);

	if (hp_globals.exception_function != NULL && strcmp(func, hp_globals.exception_function->value) == 0) {
		hp_detect_exception(func, real_execute_data TSRMLS_CC);
	}

	BEGIN_PROFILING(&hp_globals.entries, func, hp_profile_flag, real_execute_data);
#if PHP_VERSION_ID < 50500
	_zend_execute(ops TSRMLS_CC);
#else
	_zend_execute_ex(execute_data TSRMLS_CC);
#endif
	if (hp_globals.entries) {
		END_PROFILING(&hp_globals.entries, hp_profile_flag, real_execute_data);
	}
	efree(func);
}

#undef EX
#define EX(element) ((execute_data)->element)

/**
 * Very similar to hp_execute. Proxy for zend_execute_internal().
 * Applies to zend builtin functions.
 *
 * @author hzhao, kannan
 */

#if PHP_VERSION_ID < 50500
#define EX_T(offset) (*(temp_variable *)((char *) EX(Ts) + offset))

ZEND_DLEXPORT void hp_execute_internal(zend_execute_data *execute_data,
                                       int ret TSRMLS_DC) {
#else
#define EX_T(offset) (*EX_TMP_VAR(execute_data, offset))

ZEND_DLEXPORT void hp_execute_internal(zend_execute_data *execute_data,
                                       struct _zend_fcall_info *fci, int ret TSRMLS_DC) {
#endif
	char             *func = NULL;
	int    hp_profile_flag = 1;

	func = hp_get_function_name(execute_data TSRMLS_CC);

	if (func) {
		BEGIN_PROFILING(&hp_globals.entries, func, hp_profile_flag, execute_data);
	}

	if (!_zend_execute_internal) {
#if PHP_VERSION_ID < 50500
		execute_internal(execute_data, ret TSRMLS_CC);
#else
		execute_internal(execute_data, fci, ret TSRMLS_CC);
#endif
	} else {
		/* call the old override */
#if PHP_VERSION_ID < 50500
		_zend_execute_internal(execute_data, ret TSRMLS_CC);
#else
		_zend_execute_internal(execute_data, fci, ret TSRMLS_CC);
#endif
	}

	if (func) {
		if (hp_globals.entries) {
			END_PROFILING(&hp_globals.entries, hp_profile_flag, execute_data);
		}
		efree(func);
	}
}

/**
 * Proxy for zend_compile_file(). Used to profile PHP compilation time.
 *
 * @author kannan, hzhao
 */
ZEND_DLEXPORT zend_op_array* hp_compile_file(zend_file_handle *file_handle, int type TSRMLS_DC)
{
	zend_op_array  *ret;
	uint64 start = cycle_timer();

	hp_globals.compile_count++;

	ret = _zend_compile_file(file_handle, type TSRMLS_CC);

	hp_globals.compile_wt += get_us_from_tsc(cycle_timer() - start);

	return ret;
}

/**
 * Proxy for zend_compile_string(). Used to profile PHP eval compilation time.
 */
ZEND_DLEXPORT zend_op_array* hp_compile_string(zval *source_string, char *filename TSRMLS_DC)
{
	zend_op_array  *ret;
	uint64 start = cycle_timer();

	hp_globals.compile_count++;

	ret = _zend_compile_string(source_string, filename TSRMLS_CC);

	hp_globals.compile_wt += get_us_from_tsc(cycle_timer() - start);

	return ret;
}

/**
 * **************************
 * MAIN TIDEWAYS CALLBACKS
 * **************************
 */

/**
 * This function gets called once when Tideways gets enabled.
 * It replaces all the functions like zend_execute, zend_execute_internal,
 * etc that needs to be instrumented with their corresponding proxies.
 */
static void hp_begin(long tideways_flags TSRMLS_DC)
{
	if (!hp_globals.enabled) {
		int hp_profile_flag = 1;

		hp_globals.enabled      = 1;
		hp_globals.tideways_flags = (uint32)tideways_flags;

		/* Replace zend_compile file/string with our proxies */
		_zend_compile_file = zend_compile_file;
		_zend_compile_string = zend_compile_string;

		if (!(hp_globals.tideways_flags & TIDEWAYS_FLAGS_NO_COMPILE)) {
			zend_compile_file  = hp_compile_file;
			zend_compile_string = hp_compile_string;
		}

		/* Replace zend_execute with our proxy */
#if PHP_VERSION_ID < 50500
		_zend_execute = zend_execute;
#else
		_zend_execute_ex = zend_execute_ex;
#endif

		if (!(hp_globals.tideways_flags & TIDEWAYS_FLAGS_NO_USERLAND)) {
#if PHP_VERSION_ID < 50500
			zend_execute  = hp_execute;
#else
			zend_execute_ex  = hp_execute_ex;
#endif
		} else if (hp_globals.transaction_function) {
#if PHP_VERSION_ID < 50500
			zend_execute  = hp_detect_tx_execute;
#else
			zend_execute_ex  = hp_detect_tx_execute_ex;
#endif
		}

		tideways_original_error_cb = zend_error_cb;
		zend_error_cb = tideways_error_cb;

		/* Replace zend_execute_internal with our proxy */
		_zend_execute_internal = zend_execute_internal;
		if (!(hp_globals.tideways_flags & TIDEWAYS_FLAGS_NO_BUILTINS)) {
			/* if NO_BUILTINS is not set (i.e. user wants to profile builtins),
			 * then we intercept internal (builtin) function calls.
			 */
			zend_execute_internal = hp_execute_internal;
		}

		/* one time initializations */
		hp_init_profiler_state(TSRMLS_C);

		/* start profiling from fictitious main() */
		hp_globals.root = estrdup(ROOT_SYMBOL);
		hp_globals.start_time = cycle_timer();

		if ((hp_globals.tideways_flags & TIDEWAYS_FLAGS_NO_SPANS) == 0) {
			hp_globals.cpu_start = cpu_timer();
		}

		tw_span_create("app", 3);
		tw_span_timer_start(0);

		BEGIN_PROFILING(&hp_globals.entries, hp_globals.root, hp_profile_flag, NULL);
	}
}

/**
 * Called at request shutdown time. Cleans the profiler's global state.
 */
static void hp_end(TSRMLS_D)
{
	/* Bail if not ever enabled */
	if (!hp_globals.ever_enabled) {
		return;
	}

	/* Stop profiler if enabled */
	if (hp_globals.enabled) {
		hp_stop(TSRMLS_C);
	}

	/* Clean up state */
	hp_clean_profiler_state(TSRMLS_C);
}

/**
 * Called from tideways_disable(). Removes all the proxies setup by
 * hp_begin() and restores the original values.
 */
static void hp_stop(TSRMLS_D)
{
	int hp_profile_flag = 1;

	/* End any unfinished calls */
	while (hp_globals.entries) {
		END_PROFILING(&hp_globals.entries, hp_profile_flag, NULL);
	}

	tw_span_timer_stop(0);

	if ((hp_globals.tideways_flags & TIDEWAYS_FLAGS_NO_SPANS) == 0) {
		if ((GC_G(gc_runs) - hp_globals.gc_runs) > 0) {
			tw_span_annotate_long(0, "gc", GC_G(gc_runs) - hp_globals.gc_runs);
			tw_span_annotate_long(0, "gcc", GC_G(collected) - hp_globals.gc_collected);
		}

		if (hp_globals.compile_count > 0) {
			tw_span_annotate_long(0, "cct", hp_globals.compile_count);
		}
		if (hp_globals.compile_wt > 0) {
			tw_span_annotate_long(0, "cwt", hp_globals.compile_wt);
		}

		tw_span_annotate_long(0, "cpu", get_us_from_tsc(cpu_timer() - hp_globals.cpu_start));
	}

	if (hp_globals.root) {
		efree(hp_globals.root);
		hp_globals.root = NULL;
	}

	/* Remove proxies, restore the originals */
#if PHP_VERSION_ID < 50500
	zend_execute = _zend_execute;
#else
	zend_execute_ex = _zend_execute_ex;
#endif

	zend_execute_internal = _zend_execute_internal;
	zend_compile_file     = _zend_compile_file;
	zend_compile_string   = _zend_compile_string;

	zend_error_cb = tideways_original_error_cb;

	/* Stop profiling */
	hp_globals.enabled = 0;
}


/**
 * *****************************
 * TIDEWAYS ZVAL UTILITY FUNCTIONS
 * *****************************
 */

/** Look in the PHP assoc array to find a key and return the zval associated
 *  with it.
 *
 *  @author mpal
 **/
static zval *hp_zval_at_key(char  *key, zval  *values)
{
	zval *result = NULL;

	if (values->type == IS_ARRAY) {
		HashTable *ht;
		zval     **value;
		uint       len = strlen(key) + 1;

		ht = Z_ARRVAL_P(values);
		if (zend_hash_find(ht, key, len, (void**)&value) == SUCCESS) {
			result = *value;
		}
	}

	return result;
}

/**
 *  Convert the PHP array of strings to an emalloced array of strings. Note,
 *  this method duplicates the string data in the PHP array.
 *
 *  @author mpal
 **/
static char **hp_strings_in_zval(zval  *values)
{
	char   **result;
	size_t   count;
	size_t   ix = 0;

	if (!values) {
		return NULL;
	}

	if (values->type == IS_ARRAY) {
		HashTable *ht;

		ht    = Z_ARRVAL_P(values);
		count = zend_hash_num_elements(ht);

		if((result =
					(char**)emalloc(sizeof(char*) * (count + 1))) == NULL) {
			return result;
		}

		for (zend_hash_internal_pointer_reset(ht);
				zend_hash_has_more_elements(ht) == SUCCESS;
				zend_hash_move_forward(ht)) {
			char  *str;
			uint   len;
			ulong  idx;
			int    type;
			zval **data;

			type = zend_hash_get_current_key_ex(ht, &str, &len, &idx, 0, NULL);

			if (type == HASH_KEY_IS_LONG) {
				if ((zend_hash_get_current_data(ht, (void**)&data) == SUCCESS) &&
						Z_TYPE_PP(data) == IS_STRING &&
						strcmp(Z_STRVAL_PP(data), ROOT_SYMBOL)) { /* do not ignore "main" */
					result[ix] = estrdup(Z_STRVAL_PP(data));
					ix++;
				}
			} else if (type == HASH_KEY_IS_STRING) {
				result[ix] = estrdup(str);
				ix++;
			}
		}
	} else if(values->type == IS_STRING) {
		if((result = (char**)emalloc(sizeof(char*) * 2)) == NULL) {
			return result;
		}
		result[0] = estrdup(Z_STRVAL_P(values));
		ix = 1;
	} else {
		result = NULL;
	}

	/* NULL terminate the array */
	if (result != NULL) {
		result[ix] = NULL;
	}

	return result;
}

/* Free this memory at the end of profiling */
static inline void hp_array_del(char **name_array)
{
	if (name_array != NULL) {
		int i = 0;
		for(; name_array[i] != NULL && i < TIDEWAYS_MAX_FILTERED_FUNCTIONS; i++) {
			efree(name_array[i]);
		}
		efree(name_array);
	}
}

void tideways_error_cb(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args)
{
	TSRMLS_FETCH();
	error_handling_t  error_handling;
	zval *backtrace;

#if (PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION >= 3) || PHP_MAJOR_VERSION >= 6
	error_handling  = EG(error_handling);
#else
	error_handling  = PG(error_handling);
#endif

	if (error_handling == EH_NORMAL) {
		switch (type) {
			case E_ERROR:
			case E_CORE_ERROR:
				ALLOC_INIT_ZVAL(backtrace);

#if PHP_VERSION_ID <= 50399
				zend_fetch_debug_backtrace(backtrace, 1, 0 TSRMLS_CC);
#else
				zend_fetch_debug_backtrace(backtrace, 1, 0, 0 TSRMLS_CC);
#endif

				hp_globals.backtrace = backtrace;
		}
	}

	tideways_original_error_cb(type, error_filename, error_lineno, format, args);
}

static inline hp_string *hp_create_string(const char *value, size_t length)
{
	hp_string *str;

	str = emalloc(sizeof(hp_string));
	str->value = estrdup(value);
	str->length = length;

	return str;
}

static inline long hp_zval_to_long(zval *z)
{
	if (Z_TYPE_P(z) == IS_LONG) {
		return Z_LVAL_P(z);
	}

	return 0;
}

static inline hp_string *hp_zval_to_string(zval *z)
{
	if (Z_TYPE_P(z) == IS_STRING) {
		return hp_create_string(Z_STRVAL_P(z), Z_STRLEN_P(z));
	}

	return NULL;
}

static inline zval *hp_string_to_zval(hp_string *str)
{
	zval *ret;
	char *val;

	MAKE_STD_ZVAL(ret);
	ZVAL_NULL(ret);

	if (str == NULL) {
		return ret;
	}

	ZVAL_STRINGL(ret, str->value, str->length, 1);

	return ret;
}


static inline void hp_string_clean(hp_string *str)
{
	if (str == NULL) {
		return;
	}

	efree(str->value);
}

PHP_FUNCTION(tideways_span_watch)
{
	char *func = NULL, *category = NULL;
	int func_len, category_len;
	tw_trace_callback cb;

	if (!hp_globals.enabled || (hp_globals.tideways_flags & TIDEWAYS_FLAGS_NO_SPANS) > 0) {
		return;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|s!", &func, &func_len, &category, &category_len) == FAILURE) {
		return;
	}

	if (category != NULL && strcmp(category, "view") == 0) {
		cb = tw_trace_callback_view_engine;
	} else if (category != NULL && strcmp(category, "event") == 0) {
		cb = tw_trace_callback_event_dispatchers;
	} else {
		cb = tw_trace_callback_php_call;
	}

	register_trace_callback_len(func, func_len, cb);
}

static void free_tw_watch_callback(void *twcb)
{
	tw_watch_callback *_twcb = *((tw_watch_callback **)twcb);

	if (_twcb->fci.function_name) {
		zval_ptr_dtor((zval **)&_twcb->fci.function_name);
	}
	if (_twcb->fci.object_ptr) {
		zval_ptr_dtor((zval **)&_twcb->fci.object_ptr);
	}

	efree(_twcb);
}

static void tideways_add_callback_watch(zend_fcall_info fci, zend_fcall_info_cache fcic, char *func, int func_len TSRMLS_DC)
{
	tw_watch_callback *twcb;
	tw_trace_callback cb;

	twcb = emalloc(sizeof(tw_watch_callback));
	twcb->fci = fci;
	twcb->fcic = fcic;

	if (hp_globals.trace_watch_callbacks == NULL) {
		ALLOC_HASHTABLE(hp_globals.trace_watch_callbacks);
		zend_hash_init(hp_globals.trace_watch_callbacks, 255, NULL, free_tw_watch_callback, 0);
	}

	zend_hash_update(hp_globals.trace_watch_callbacks, func, func_len+1, &twcb, sizeof(tw_watch_callback*), NULL);
	cb = tw_trace_callback_watch;
	register_trace_callback_len(func, func_len, cb);
}

PHP_FUNCTION(tideways_span_callback)
{
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcic = empty_fcall_info_cache;
	char *func;
	int func_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sf", &func, &func_len, &fci, &fcic) == FAILURE) {
		zend_error(E_ERROR, "tideways_callback_watch() expects a string as a first and a callback as a second argument");
		return;
	}

	if (fci.size) {
		Z_ADDREF_P(fci.function_name);
		if (fci.object_ptr) {
			Z_ADDREF_P(fci.object_ptr);
		}
	}

	tideways_add_callback_watch(fci, fcic, func, func_len TSRMLS_CC);
}
