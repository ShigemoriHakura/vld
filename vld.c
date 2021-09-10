/*
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2019 Derick Rethans                               |
   +----------------------------------------------------------------------+
   | This source file is subject to the 2-Clause BSD license which is     |
   | available through the LICENSE file, or online at                     |
   | http://opensource.org/licenses/bsd-license.php                       |
   +----------------------------------------------------------------------+
   | Authors:  Derick Rethans <derick@derickrethans.nl>                   |
   +----------------------------------------------------------------------+
 */
/* $Id: vld.c,v 1.40 2009-03-30 18:36:55 derick Exp $ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/standard/url.h"
#include "php_vld.h"
#include "srm_oparray.h"
#include "php_globals.h"

static zend_op_array* (*old_compile_file)(zend_file_handle* file_handle, int type);
static zend_op_array* vld_compile_file(zend_file_handle*, int);

#if PHP_VERSION_ID < 80000
static zend_op_array* (*old_compile_string)(zval *source_string, char *filename);
static zend_op_array* vld_compile_string(zval *source_string, char *filename);
#else
static zend_op_array* (*old_compile_string)(zend_string *source_string, const char *filename);
static zend_op_array* vld_compile_string(zend_string *source_string, const char *filename);
#endif

static void (*old_execute_ex)(zend_execute_data *execute_data);
static void vld_execute_ex(zend_execute_data *execute_data);

/* {{{ forward declarations */
static int vld_check_fe (zend_op_array *fe, zend_bool *have_fe);
static int vld_dump_fe (zend_op_array *fe, int num_args, va_list args, zend_hash_key *hash_key);
static int vld_dump_cle (zend_class_entry *class_entry);
/* }}} */

zend_function_entry vld_functions[] = {
	ZEND_FE_END
};


zend_module_entry vld_module_entry = {
	STANDARD_MODULE_HEADER,
	"vld",
	vld_functions,
	PHP_MINIT(vld),
	PHP_MSHUTDOWN(vld),
	PHP_RINIT(vld),
	PHP_RSHUTDOWN(vld),
	PHP_MINFO(vld),
	"0.17.1",
	STANDARD_MODULE_PROPERTIES
};


#ifdef COMPILE_DL_VLD
ZEND_GET_MODULE(vld)
#endif

ZEND_DECLARE_MODULE_GLOBALS(vld)

PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("vld.active",       "0", PHP_INI_SYSTEM, OnUpdateBool, active,       zend_vld_globals, vld_globals)
	STD_PHP_INI_ENTRY("vld.skip_prepend", "0", PHP_INI_SYSTEM, OnUpdateBool, skip_prepend, zend_vld_globals, vld_globals)
	STD_PHP_INI_ENTRY("vld.skip_append",  "0", PHP_INI_SYSTEM, OnUpdateBool, skip_append,  zend_vld_globals, vld_globals)
	STD_PHP_INI_ENTRY("vld.execute",      "1", PHP_INI_SYSTEM, OnUpdateBool, execute,      zend_vld_globals, vld_globals)
	STD_PHP_INI_ENTRY("vld.verbosity",    "1", PHP_INI_SYSTEM, OnUpdateLong, verbosity,    zend_vld_globals, vld_globals)
	STD_PHP_INI_ENTRY("vld.format",       "0", PHP_INI_SYSTEM, OnUpdateBool, format,       zend_vld_globals, vld_globals)
	STD_PHP_INI_ENTRY("vld.col_sep",      "\t", PHP_INI_SYSTEM, OnUpdateString, col_sep,   zend_vld_globals, vld_globals)
	STD_PHP_INI_ENTRY("vld.save_dir",     "/tmp", PHP_INI_SYSTEM, OnUpdateString, save_dir, zend_vld_globals, vld_globals)
	STD_PHP_INI_ENTRY("vld.save_paths",   "0", PHP_INI_SYSTEM, OnUpdateBool, save_paths,   zend_vld_globals, vld_globals)
	STD_PHP_INI_ENTRY("vld.dump_paths",   "1", PHP_INI_SYSTEM, OnUpdateBool, dump_paths,   zend_vld_globals, vld_globals)
	STD_PHP_INI_ENTRY("vld.sg_decode",    "0", PHP_INI_SYSTEM, OnUpdateBool, sg_decode,    zend_vld_globals, vld_globals)
PHP_INI_END()

static void vld_init_globals(zend_vld_globals *vg)
{
	vg->active       = 0;
	vg->skip_prepend = 0;
	vg->skip_append  = 0;
	vg->execute      = 1;
	vg->format       = 0;
	vg->col_sep      = (char*) "\t";
	vg->path_dump_file = NULL;
	vg->dump_paths   = 1;
	vg->save_paths   = 0;
	vg->verbosity    = 1;
	vg->sg_decode    = 0;
}


PHP_MINIT_FUNCTION(vld)
{
	ZEND_INIT_MODULE_GLOBALS(vld, vld_init_globals, NULL);
	REGISTER_INI_ENTRIES();

	return SUCCESS;
}


PHP_MSHUTDOWN_FUNCTION(vld)
{
	UNREGISTER_INI_ENTRIES();

	zend_compile_file   = old_compile_file;
	zend_compile_string = old_compile_string;
	zend_execute_ex     = old_execute_ex;

	return SUCCESS;
}



PHP_RINIT_FUNCTION(vld)
{
	old_compile_file = zend_compile_file;
	old_compile_string = zend_compile_string;
	old_execute_ex = zend_execute_ex;

	if (VLD_G(active)) {
		zend_compile_file = vld_compile_file;
		zend_compile_string = vld_compile_string;
		if (!VLD_G(execute)) {
			zend_execute_ex = vld_execute_ex;
		}
	}

	if (VLD_G(save_paths)) {
		char *filename;

		filename = malloc(strlen("paths.dot") + strlen(VLD_G(save_dir)) + 2);
		sprintf(filename, "%s/%s", VLD_G(save_dir), "paths.dot");

		VLD_G(path_dump_file) = fopen(filename, "w");
		free(filename);

		if (VLD_G(path_dump_file)) {
			fprintf(VLD_G(path_dump_file), "digraph {\n");
		}
	}
	return SUCCESS;
}



PHP_RSHUTDOWN_FUNCTION(vld)
{
	zend_compile_file   = old_compile_file;
	zend_compile_string = old_compile_string;
	zend_execute_ex     = old_execute_ex;

	if (VLD_G(path_dump_file)) {
		fprintf(VLD_G(path_dump_file), "}\n");
		fclose(VLD_G(path_dump_file));
	}

	return SUCCESS;
}


PHP_MINFO_FUNCTION(vld)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "vld support", "enabled");
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();

}

/* {{{ PHP 7 wrappers */
#define VLD_WRAP_PHP7(name) name ## _wrapper

static int vld_check_fe_wrapper (zval *el, zend_bool *have_fe)
{
	return vld_check_fe((zend_op_array *) Z_PTR_P(el), have_fe);
}

static int vld_dump_fe_wrapper(zval *el, int num_args, va_list args, zend_hash_key *hash_key)
{
	return vld_dump_fe((zend_op_array *) Z_PTR_P(el), num_args, args, hash_key);
}

static int vld_dump_cle_wrapper (zval *el)
{
	return vld_dump_cle((zend_class_entry *) Z_PTR_P(el));
}
/* }}} */

int vld_printf(FILE *stream, const char* fmt, ...)
{
	char *message;
	int len;
	va_list args;
	int i = 0;
	size_t j = 0;
	char *ptr;
	const char EOL='\n';

	va_start(args, fmt);
	len = vspprintf(&message, 0, fmt, args);
	va_end(args);
	if (VLD_G(format)) {
		ptr = message;
		while (j < strlen(ptr)) {
			if (!isspace(ptr[j]) || ptr[j] == EOL) {
				ptr[i++] = ptr[j];
			}
			j++;
		}
		ptr[i] = 0;

		fprintf(stream, "%s%s", VLD_G(col_sep), ptr);
	} else {
		fprintf(stream, "%s", message);
	}

	efree(message);

	return len;
}

static int vld_check_fe (zend_op_array *fe, zend_bool *have_fe)
{
	if (fe->type == ZEND_USER_FUNCTION) {
		*have_fe = 1;
	}

	return 0;
}

static int vld_dump_fe (zend_op_array *fe, int num_args, va_list args, zend_hash_key *hash_key)
{
	if (fe->type == ZEND_USER_FUNCTION) {
		ZVAL_VALUE_STRING_TYPE *new_str;

		new_str = php_url_encode(ZHASHKEYSTR(hash_key), ZHASHKEYLEN(hash_key) PHP_URLENCODE_NEW_LEN(new_len));
		vld_printf(stderr, "Function %s:\n", ZSTRING_VALUE(new_str));
		vld_dump_oparray(fe);
		vld_printf(stderr, "End of function %s\n\n", ZSTRING_VALUE(new_str));
		efree(new_str);
	}

	return ZEND_HASH_APPLY_KEEP;
}


static int vld_dump_cle (zend_class_entry *class_entry)
{
	zend_class_entry *ce;
	zend_bool have_fe = 0;
	ce = class_entry;

	if (ce->type != ZEND_INTERNAL_CLASS) {
		if (VLD_G(path_dump_file)) {
			fprintf(VLD_G(path_dump_file), "subgraph cluster_class_%s { label=\"class %s\";\n", ZSTRING_VALUE(ce->name), ZSTRING_VALUE(ce->name));
		}

		zend_hash_apply_with_argument(&ce->function_table, (apply_func_arg_t) VLD_WRAP_PHP7(vld_check_fe), (void *)&have_fe);

		if (have_fe) {
			vld_printf(stderr, "Class %s:\n", ZSTRING_VALUE(ce->name));
			zend_hash_apply_with_arguments(&ce->function_table, (apply_func_args_t) VLD_WRAP_PHP7(vld_dump_fe), 0);
			vld_printf(stderr, "End of class %s.\n\n", ZSTRING_VALUE(ce->name));
		} else {
			vld_printf(stderr, "Class %s: [no user functions]\n", ZSTRING_VALUE(ce->name));
		}

		if (VLD_G(path_dump_file)) {
			fprintf(VLD_G(path_dump_file), "}\n");
		}
	}

	return ZEND_HASH_APPLY_KEEP;
}


/* {{{ zend_op_array vld_compile_file (file_handle, type)
 *    This function provides a hook for compilation */
static zend_op_array *vld_compile_file(zend_file_handle *file_handle, int type)
{
	zend_op_array *op_array;

	if (!VLD_G(execute) &&
		((VLD_G(skip_prepend) && PG(auto_prepend_file) && PG(auto_prepend_file)[0] && PG(auto_prepend_file) == file_handle->filename) ||
	     (VLD_G(skip_append)  && PG(auto_append_file)  && PG(auto_append_file)[0]  && PG(auto_append_file)  == file_handle->filename)))
	{
		zend_op_array *ret;
#if PHP_VERSION_ID < 80000
		zval nop;

		ZVAL_STRINGL(&nop, "RETURN ;", 8);
		ret = compile_string(&nop, (char*) "NOP");
		zval_dtor(&nop);
#else
		zend_string *nop = zend_string_init("RETURN ;", 8, 0);

		ret = compile_string(nop, (const char*) "NOP");
		zend_string_release(nop);
#endif
		return ret;
	}

	op_array = old_compile_file (file_handle, type);

	if (VLD_G(path_dump_file)) {
		fprintf(VLD_G(path_dump_file), "subgraph cluster_file_%p { label=\"file %s\";\n", op_array, op_array->filename ? ZSTRING_VALUE(op_array->filename) : "__main");
	}
	if (op_array) {
		//vld_dump_oparray (op_array);
	}

	zend_hash_apply_with_arguments (CG(function_table), (apply_func_args_t) VLD_WRAP_PHP7(vld_dump_fe), 0);
	zend_hash_apply (CG(class_table), (apply_func_t) VLD_WRAP_PHP7(vld_dump_cle));

	if (VLD_G(path_dump_file)) {
		fprintf(VLD_G(path_dump_file), "}\n");
	}

	return op_array;
}
/* }}} */

/* {{{ zend_op_array vld_compile_string (source_string, filename)
 *    This function provides a hook for compilation */
#if PHP_VERSION_ID < 80000
static zend_op_array *vld_compile_string(zval *source_string, char *filename)
#else
static zend_op_array *vld_compile_string(zend_string *source_string, const char *filename)
#endif
{
	zend_op_array *op_array;

	op_array = old_compile_string (source_string, filename);

	if (op_array) {
		vld_dump_oparray (op_array);

		zend_hash_apply_with_arguments (CG(function_table), (apply_func_args_t) vld_dump_fe_wrapper, 0);
		zend_hash_apply (CG(class_table), (apply_func_t) vld_dump_cle_wrapper);
	}

	return op_array;
}
/* }}} */

static void _dump_function_table(const HashTable *function_table, int level)
{
    zend_function *func;

    ZEND_HASH_FOREACH_PTR(function_table, func) {
        if (level > 0) {
            php_printf("%*c", level, ' ');
        }
        zend_string *function_name = func->common.function_name;
        if (function_name) {
            php_printf("%s()", ZSTR_VAL(function_name));
        } else {
            php_printf("**unknown function**()");
        }
        php_printf("\n");
        vld_dump_oparray(&func->op_array);
        //ulop_dump_oparray_header(&func->op_array);
        //for (ii = 0; ii < &func->op_array->last; ii++) {
        //   ulop_dump_opline(&func->op_array->opcodes[ii], ii);
        //}
        //ulop_dump_oparray_footer(&func->op_array);

    } ZEND_HASH_FOREACH_END();
    return;
}

static int _in_SYS_array(const char * strr){
    int i1;
    char *php_inside[] = {
        "stdClass",
        "Traversable",
        "IteratorAggregate",
        "Iterator",
        "ArrayAccess",
        "Serializable",
        "Throwable",
        "Exception",
        "ErrorException",
        "Error",
        "ParseError",
        "TypeError",
        "ArithmeticError",
        "DivisionByZeroError",
        "Closure",
        "Generator",
        "ClosedGeneratorException",
        "DateTimeInterface",
        "DateTime",
        "DateTimeImmutable",
        "DateTimeZone",
        "DateInterval",
        "DatePeriod",
        "LibXMLError",
        "SQLite3",
        "SQLite3Stmt",
        "SQLite3Result",
        "CURLFile",
        "DOMException",
        "DOMStringList",
        "DOMNameList",
        "DOMImplementationList",
        "DOMImplementationSource",
        "DOMImplementation",
        "DOMNode",
        "DOMNameSpaceNode",
        "DOMDocumentFragment",
        "DOMDocument",
        "DOMNodeList",
        "DOMNamedNodeMap",
        "DOMCharacterData",
        "DOMAttr",
        "DOMElement",
        "DOMText",
        "DOMComment",
        "DOMTypeinfo",
        "DOMUserDataHandler",
        "DOMDomError",
        "DOMErrorHandler",
        "DOMLocator",
        "DOMConfiguration",
        "DOMCdataSection",
        "DOMDocumentType",
        "DOMNotation",
        "DOMEntity",
        "DOMEntityReference",
        "DOMProcessingInstruction",
        "DOMStringExtend",
        "DOMXPath",
        "JsonSerializable",
        "LogicException",
        "BadFunctionCallException",
        "BadMethodCallException",
        "DomainException",
        "InvalidArgumentException",
        "LengthException",
        "OutOfRangeException",
        "RuntimeException",
        "OutOfBoundsException",
        "OverflowException",
        "RangeException",
        "UnderflowException",
        "UnexpectedValueException",
        "RecursiveIterator",
        "RecursiveIteratorIterator",
        "OuterIterator",
        "IteratorIterator",
        "FilterIterator",
        "RecursiveFilterIterator",
        "CallbackFilterIterator",
        "RecursiveCallbackFilterIterator",
        "ParentIterator",
        "Countable",
        "SeekableIterator",
        "LimitIterator",
        "CachingIterator",
        "RecursiveCachingIterator",
        "NoRewindIterator",
        "AppendIterator",
        "InfiniteIterator",
        "RegexIterator",
        "RecursiveRegexIterator",
        "EmptyIterator",
        "RecursiveTreeIterator",
        "ArrayObject",
        "ArrayIterator",
        "RecursiveArrayIterator",
        "SplFileInfo",
        "DirectoryIterator",
        "FilesystemIterator",
        "RecursiveDirectoryIterator",
        "GlobIterator",
        "SplFileObject",
        "SplTempFileObject",
        "SplDoublyLinkedList",
        "SplQueue",
        "SplStack",
        "SplHeap",
        "SplMinHeap",
        "SplMaxHeap",
        "SplPriorityQueue",
        "SplFixedArray",
        "SplObserver",
        "SplSubject",
        "SplObjectStorage",
        "MultipleIterator",
        "SessionHandlerInterface",
        "SessionIdInterface",
        "SessionUpdateTimestampHandlerInterface",
        "SessionHandler",
        "__PHP_Incomplete_Class",
        "php_user_filter",
        "Directory",
        "AssertionError",
        "PDOException",
        "PDO",
        "PDOStatement",
        "PDORow",
        "PharException",
        "Phar",
        "PharData",
        "PharFileInfo",
        "ReflectionException",
        "Reflection",
        "Reflector",
        "ReflectionFunctionAbstract",
        "ReflectionFunction",
        "ReflectionGenerator",
        "ReflectionParameter",
        "ReflectionType",
        "ReflectionMethod",
        "ReflectionClass",
        "ReflectionObject",
        "ReflectionProperty",
        "ReflectionExtension",
        "ReflectionZendExtension",
        "SimpleXMLElement",
        "SimpleXMLIterator",
        "SoapClient",
        "SoapVar",
        "SoapServer",
        "SoapFault",
        "SoapParam",
        "SoapHeader",
        "mysqli_sql_exception",
        "mysqli_driver",
        "mysqli",
        "mysqli_warning",
        "mysqli_result",
        "mysqli_stmt",
        "XMLReader",
        "XMLWriter",
        "ZipArchive",
        "stdClass",
        "Traversable",
        "IteratorAggregate",
        "Iterator",
        "ArrayAccess",
        "Serializable",
        "Throwable",
        "Exception",
        "ErrorException",
        "Error",
        "ParseError",
        "TypeError",
        "ArithmeticError",
        "DivisionByZeroError",
        "Closure",
        "Generator",
        "ClosedGeneratorException",
        "DateTimeInterface",
        "DateTime",
        "DateTimeImmutable",
        "DateTimeZone",
        "DateInterval",
        "DatePeriod",
        "LibXMLError",
        "SQLite3",
        "SQLite3Stmt",
        "SQLite3Result",
        "CURLFile",
        "DOMException",
        "DOMStringList",
        "DOMNameList",
        "DOMImplementationList",
        "DOMImplementationSource",
        "DOMImplementation",
        "DOMNode",
        "DOMNameSpaceNode",
        "DOMDocumentFragment",
        "DOMDocument",
        "DOMNodeList",
        "DOMNamedNodeMap",
        "DOMCharacterData",
        "DOMAttr",
        "DOMElement",
        "DOMText",
        "DOMComment",
        "DOMTypeinfo",
        "DOMUserDataHandler",
        "DOMDomError",
        "DOMErrorHandler",
        "DOMLocator",
        "DOMConfiguration",
        "DOMCdataSection",
        "DOMDocumentType",
        "DOMNotation",
        "DOMEntity",
        "DOMEntityReference",
        "DOMProcessingInstruction",
        "DOMStringExtend",
        "DOMXPath",
        "JsonSerializable",
        "LogicException",
        "BadFunctionCallException",
        "BadMethodCallException",
        "DomainException",
        "InvalidArgumentException",
        "LengthException",
        "OutOfRangeException",
        "RuntimeException",
        "OutOfBoundsException",
        "OverflowException",
        "RangeException",
        "UnderflowException",
        "UnexpectedValueException",
        "RecursiveIterator",
        "RecursiveIteratorIterator",
        "OuterIterator",
        "IteratorIterator",
        "FilterIterator",
        "RecursiveFilterIterator",
        "CallbackFilterIterator",
        "RecursiveCallbackFilterIterator",
        "ParentIterator",
        "Countable",
        "SeekableIterator",
        "LimitIterator",
        "CachingIterator",
        "RecursiveCachingIterator",
        "NoRewindIterator",
        "AppendIterator",
        "InfiniteIterator",
        "RegexIterator",
        "RecursiveRegexIterator",
        "EmptyIterator",
        "RecursiveTreeIterator",
        "ArrayObject",
        "ArrayIterator",
        "RecursiveArrayIterator",
        "SplFileInfo",
        "DirectoryIterator",
        "FilesystemIterator",
        "RecursiveDirectoryIterator",
        "GlobIterator",
        "SplFileObject",
        "SplTempFileObject",
        "SplDoublyLinkedList",
        "SplQueue",
        "SplStack",
        "SplHeap",
        "SplMinHeap",
        "SplMaxHeap",
        "SplPriorityQueue",
        "SplFixedArray",
        "SplObserver",
        "SplSubject",
        "SplObjectStorage",
        "MultipleIterator",
        "SessionHandlerInterface",
        "SessionIdInterface",
        "SessionUpdateTimestampHandlerInterface",
        "SessionHandler",
        "__PHP_Incomplete_Class",
        "php_user_filter",
        "Directory",
        "AssertionError",
        "PDOException",
        "PDO",
        "PDOStatement",
        "PDORow",
        "PharException",
        "Phar",
        "PharData",
        "PharFileInfo",
        "ReflectionException",
        "Reflection",
        "Reflector",
        "ReflectionFunctionAbstract",
        "ReflectionFunction",
        "ReflectionGenerator",
        "ReflectionParameter",
        "ReflectionType",
        "ReflectionMethod",
        "ReflectionClass",
        "ReflectionObject",
        "ReflectionProperty",
        "ReflectionExtension",
        "ReflectionZendExtension",
        "SimpleXMLElement",
        "SimpleXMLIterator",
        "SoapClient",
        "SoapVar",
        "SoapServer",
        "SoapFault",
        "SoapParam",
        "SoapHeader",
        "mysqli_sql_exception",
        "mysqli_driver",
        "mysqli",
        "mysqli_warning",
        "mysqli_result",
        "mysqli_stmt",
        "XMLReader",
        "XMLWriter",
        "ZipArchive",
        "ArgumentCountError",
        "ReflectionNamedType",
        "ReflectionClassConstant",
        "ArgumentCountError",
				"CompileError",
				"WeakReference",
				"HashContext",
				"Collator",
				"NumberFormatter",
				"Normalizer",
				"Locale",
				"MessageFormatter",
				"IntlDateFormatter",
				"ResourceBundle",
				"Transliterator",
				"IntlTimeZone",
				"IntlCalendar",
				"IntlGregorianCalendar",
				"Spoofchecker",
				"IntlException",
				"IntlIterator",
				"IntlBreakIterator",
				"IntlRuleBasedBreakIterator",
				"IntlCodePointBreakIterator",
				"IntlPartsIterator",
				"UConverter",
				"IntlChar",
				"JsonException",
				"ReflectionReference",
				"SodiumException",
    };
    for(i1 = 0; i1 < 356; ++i1){
        char str1[99]; 
        sprintf(str1, "%s", php_inside[i1]); 
        if(strcmp(strr, str1) == 0){
            return 1;
        }
    }
    return 0;
}

static int _in_SYS_Func_array(const char * str3){
    int i2;
    char *php_inside_func[] = {
        "zend_version",
        "func_num_args",
        "func_get_arg",
        "func_get_args",
        "strlen",
        "strcmp",
        "strncmp",
        "strcasecmp",
        "strncasecmp",
        "each",
        "error_reporting",
        "define",
        "defined",
        "get_class",
        "get_called_class",
        "get_parent_class",
        "method_exists",
        "property_exists",
        "class_exists",
        "interface_exists",
        "trait_exists",
        "function_exists",
        "class_alias",
        "get_included_files",
        "get_required_files",
        "is_subclass_of",
        "is_a",
        "get_class_vars",
        "get_object_vars",
        "get_class_methods",
        "trigger_error",
        "user_error",
        "set_error_handler",
        "restore_error_handler",
        "set_exception_handler",
        "restore_exception_handler",
        "get_declared_classes",
        "get_declared_traits",
        "get_declared_interfaces",
        "get_defined_functions",
        "get_defined_vars",
        "create_function",
        "get_resource_type",
        "get_resources",
        "get_loaded_extensions",
        "extension_loaded",
        "get_extension_funcs",
        "get_defined_constants",
        "debug_backtrace",
        "debug_print_backtrace",
        "gc_mem_caches",
        "gc_collect_cycles",
        "gc_enabled",
        "gc_enable",
        "gc_disable",
        "strtotime",
        "date",
        "idate",
        "gmdate",
        "mktime",
        "gmmktime",
        "checkdate",
        "strftime",
        "gmstrftime",
        "time",
        "localtime",
        "getdate",
        "date_create",
        "date_create_immutable",
        "date_create_from_format",
        "date_create_immutable_from_format",
        "date_parse",
        "date_parse_from_format",
        "date_get_last_errors",
        "date_format",
        "date_modify",
        "date_add",
        "date_sub",
        "date_timezone_get",
        "date_timezone_set",
        "date_offset_get",
        "date_diff",
        "date_time_set",
        "date_date_set",
        "date_isodate_set",
        "date_timestamp_set",
        "date_timestamp_get",
        "timezone_open",
        "timezone_name_get",
        "timezone_name_from_abbr",
        "timezone_offset_get",
        "timezone_transitions_get",
        "timezone_location_get",
        "timezone_identifiers_list",
        "timezone_abbreviations_list",
        "timezone_version_get",
        "date_interval_create_from_date_string",
        "date_interval_format",
        "date_default_timezone_set",
        "date_default_timezone_get",
        "date_sunrise",
        "date_sunset",
        "date_sun_info",
        "libxml_set_streams_context",
        "libxml_use_internal_errors",
        "libxml_get_last_error",
        "libxml_clear_errors",
        "libxml_get_errors",
        "libxml_disable_entity_loader",
        "libxml_set_external_entity_loader",
        "preg_match",
        "preg_match_all",
        "preg_replace",
        "preg_replace_callback",
        "preg_replace_callback_array",
        "preg_filter",
        "preg_split",
        "preg_quote",
        "preg_grep",
        "preg_last_error",
        "readgzfile",
        "gzrewind",
        "gzclose",
        "gzeof",
        "gzgetc",
        "gzgets",
        "gzgetss",
        "gzread",
        "gzopen",
        "gzpassthru",
        "gzseek",
        "gztell",
        "gzwrite",
        "gzputs",
        "gzfile",
        "gzcompress",
        "gzuncompress",
        "gzdeflate",
        "gzinflate",
        "gzencode",
        "gzdecode",
        "zlib_encode",
        "zlib_decode",
        "zlib_get_coding_type",
        "deflate_init",
        "deflate_add",
        "inflate_init",
        "inflate_add",
        "ob_gzhandler",
        "bcadd",
        "bcsub",
        "bcmul",
        "bcdiv",
        "bcmod",
        "bcpow",
        "bcsqrt",
        "bcscale",
        "bccomp",
        "bcpowmod",
        "ctype_alnum",
        "ctype_alpha",
        "ctype_cntrl",
        "ctype_digit",
        "ctype_lower",
        "ctype_graph",
        "ctype_print",
        "ctype_punct",
        "ctype_space",
        "ctype_upper",
        "ctype_xdigit",
        "curl_init",
        "curl_copy_handle",
        "curl_version",
        "curl_setopt",
        "curl_setopt_array",
        "curl_exec",
        "curl_getinfo",
        "curl_error",
        "curl_errno",
        "curl_close",
        "curl_strerror",
        "curl_multi_strerror",
        "curl_share_strerror",
        "curl_reset",
        "curl_escape",
        "curl_unescape",
        "curl_pause",
        "curl_multi_init",
        "curl_multi_add_handle",
        "curl_multi_remove_handle",
        "curl_multi_select",
        "curl_multi_exec",
        "curl_multi_getcontent",
        "curl_multi_info_read",
        "curl_multi_close",
        "curl_multi_errno",
        "curl_multi_setopt",
        "curl_share_init",
        "curl_share_close",
        "curl_share_setopt",
        "curl_share_errno",
        "curl_file_create",
        "dom_import_simplexml",
        "filter_input",
        "filter_var",
        "filter_input_array",
        "filter_var_array",
        "filter_list",
        "filter_has_var",
        "filter_id",
        "ftp_connect",
        "ftp_login",
        "ftp_pwd",
        "ftp_cdup",
        "ftp_chdir",
        "ftp_exec",
        "ftp_raw",
        "ftp_mkdir",
        "ftp_rmdir",
        "ftp_chmod",
        "ftp_alloc",
        "ftp_nlist",
        "ftp_rawlist",
        "ftp_systype",
        "ftp_pasv",
        "ftp_get",
        "ftp_fget",
        "ftp_put",
        "ftp_fput",
        "ftp_size",
        "ftp_mdtm",
        "ftp_rename",
        "ftp_delete",
        "ftp_site",
        "ftp_close",
        "ftp_set_option",
        "ftp_get_option",
        "ftp_nb_fget",
        "ftp_nb_get",
        "ftp_nb_continue",
        "ftp_nb_put",
        "ftp_nb_fput",
        "ftp_quit",
        "gd_info",
        "imagearc",
        "imageellipse",
        "imagechar",
        "imagecharup",
        "imagecolorat",
        "imagecolorallocate",
        "imagepalettecopy",
        "imagecreatefromstring",
        "imagecolorclosest",
        "imagecolorclosesthwb",
        "imagecolordeallocate",
        "imagecolorresolve",
        "imagecolorexact",
        "imagecolorset",
        "imagecolortransparent",
        "imagecolorstotal",
        "imagecolorsforindex",
        "imagecopy",
        "imagecopymerge",
        "imagecopymergegray",
        "imagecopyresized",
        "imagecreate",
        "imagecreatetruecolor",
        "imageistruecolor",
        "imagetruecolortopalette",
        "imagepalettetotruecolor",
        "imagesetthickness",
        "imagefilledarc",
        "imagefilledellipse",
        "imagealphablending",
        "imagesavealpha",
        "imagecolorallocatealpha",
        "imagecolorresolvealpha",
        "imagecolorclosestalpha",
        "imagecolorexactalpha",
        "imagecopyresampled",
        "imagerotate",
        "imageflip",
        "imageantialias",
        "imagecrop",
        "imagecropauto",
        "imagescale",
        "imageaffine",
        "imageaffinematrixconcat",
        "imageaffinematrixget",
        "imagesetinterpolation",
        "imagesettile",
        "imagesetbrush",
        "imagesetstyle",
        "imagecreatefrompng",
        "imagecreatefromgif",
        "imagecreatefromjpeg",
        "imagecreatefromwbmp",
        "imagecreatefromxbm",
        "imagecreatefromgd",
        "imagecreatefromgd2",
        "imagecreatefromgd2part",
        "imagepng",
        "imagegif",
        "imagejpeg",
        "imagewbmp",
        "imagegd",
        "imagegd2",
        "imagedestroy",
        "imagegammacorrect",
        "imagefill",
        "imagefilledpolygon",
        "imagefilledrectangle",
        "imagefilltoborder",
        "imagefontwidth",
        "imagefontheight",
        "imageinterlace",
        "imageline",
        "imageloadfont",
        "imagepolygon",
        "imagerectangle",
        "imagesetpixel",
        "imagestring",
        "imagestringup",
        "imagesx",
        "imagesy",
        "imagedashedline",
        "imagettfbbox",
        "imagettftext",
        "imageftbbox",
        "imagefttext",
        "imagetypes",
        "jpeg2wbmp",
        "png2wbmp",
        "image2wbmp",
        "imagelayereffect",
        "imagexbm",
        "imagecolormatch",
        "imagefilter",
        "imageconvolution",
        "textdomain",
        "gettext",
        "_",
        "dgettext",
        "dcgettext",
        "bindtextdomain",
        "ngettext",
        "dngettext",
        "dcngettext",
        "bind_textdomain_codeset",
        "hash",
        "hash_file",
        "hash_hmac",
        "hash_hmac_file",
        "hash_init",
        "hash_update",
        "hash_update_stream",
        "hash_update_file",
        "hash_final",
        "hash_copy",
        "hash_algos",
        "hash_pbkdf2",
        "hash_equals",
        "hash_hkdf",
        "mhash_keygen_s2k",
        "mhash_get_block_size",
        "mhash_get_hash_name",
        "mhash_count",
        "mhash",
        "iconv",
        "iconv_get_encoding",
        "iconv_set_encoding",
        "iconv_strlen",
        "iconv_substr",
        "iconv_strpos",
        "iconv_strrpos",
        "iconv_mime_encode",
        "iconv_mime_decode",
        "iconv_mime_decode_headers",
        "json_encode",
        "json_decode",
        "json_last_error",
        "json_last_error_msg",
        "mb_convert_case",
        "mb_strtoupper",
        "mb_strtolower",
        "mb_language",
        "mb_internal_encoding",
        "mb_http_input",
        "mb_http_output",
        "mb_detect_order",
        "mb_substitute_character",
        "mb_parse_str",
        "mb_output_handler",
        "mb_preferred_mime_name",
        "mb_strlen",
        "mb_strpos",
        "mb_strrpos",
        "mb_stripos",
        "mb_strripos",
        "mb_strstr",
        "mb_strrchr",
        "mb_stristr",
        "mb_strrichr",
        "mb_substr_count",
        "mb_substr",
        "mb_strcut",
        "mb_strwidth",
        "mb_strimwidth",
        "mb_convert_encoding",
        "mb_detect_encoding",
        "mb_list_encodings",
        "mb_encoding_aliases",
        "mb_convert_kana",
        "mb_encode_mimeheader",
        "mb_decode_mimeheader",
        "mb_convert_variables",
        "mb_encode_numericentity",
        "mb_decode_numericentity",
        "mb_send_mail",
        "mb_get_info",
        "mb_check_encoding",
        "mb_regex_encoding",
        "mb_regex_set_options",
        "mb_ereg",
        "mb_eregi",
        "mb_ereg_replace",
        "mb_eregi_replace",
        "mb_ereg_replace_callback",
        "mb_split",
        "mb_ereg_match",
        "mb_ereg_search",
        "mb_ereg_search_pos",
        "mb_ereg_search_regs",
        "mb_ereg_search_init",
        "mb_ereg_search_getregs",
        "mb_ereg_search_getpos",
        "mb_ereg_search_setpos",
        "mbregex_encoding",
        "mbereg",
        "mberegi",
        "mbereg_replace",
        "mberegi_replace",
        "mbsplit",
        "mbereg_match",
        "mbereg_search",
        "mbereg_search_pos",
        "mbereg_search_regs",
        "mbereg_search_init",
        "mbereg_search_getregs",
        "mbereg_search_getpos",
        "mbereg_search_setpos",
        "mcrypt_get_key_size",
        "mcrypt_get_block_size",
        "mcrypt_get_cipher_name",
        "mcrypt_create_iv",
        "mcrypt_list_algorithms",
        "mcrypt_list_modes",
        "mcrypt_get_iv_size",
        "mcrypt_encrypt",
        "mcrypt_decrypt",
        "mcrypt_module_open",
        "mcrypt_generic_init",
        "mcrypt_generic",
        "mdecrypt_generic",
        "mcrypt_generic_deinit",
        "mcrypt_enc_self_test",
        "mcrypt_enc_is_block_algorithm_mode",
        "mcrypt_enc_is_block_algorithm",
        "mcrypt_enc_is_block_mode",
        "mcrypt_enc_get_block_size",
        "mcrypt_enc_get_key_size",
        "mcrypt_enc_get_supported_key_sizes",
        "mcrypt_enc_get_iv_size",
        "mcrypt_enc_get_algorithms_name",
        "mcrypt_enc_get_modes_name",
        "mcrypt_module_self_test",
        "mcrypt_module_is_block_algorithm_mode",
        "mcrypt_module_is_block_algorithm",
        "mcrypt_module_is_block_mode",
        "mcrypt_module_get_algo_block_size",
        "mcrypt_module_get_algo_key_size",
        "mcrypt_module_get_supported_key_sizes",
        "mcrypt_module_close",
        "mysqli_affected_rows",
        "mysqli_autocommit",
        "mysqli_begin_transaction",
        "mysqli_change_user",
        "mysqli_character_set_name",
        "mysqli_close",
        "mysqli_commit",
        "mysqli_connect",
        "mysqli_connect_errno",
        "mysqli_connect_error",
        "mysqli_data_seek",
        "mysqli_dump_debug_info",
        "mysqli_debug",
        "mysqli_errno",
        "mysqli_error",
        "mysqli_error_list",
        "mysqli_stmt_execute",
        "mysqli_execute",
        "mysqli_fetch_field",
        "mysqli_fetch_fields",
        "mysqli_fetch_field_direct",
        "mysqli_fetch_lengths",
        "mysqli_fetch_all",
        "mysqli_fetch_array",
        "mysqli_fetch_assoc",
        "mysqli_fetch_object",
        "mysqli_fetch_row",
        "mysqli_field_count",
        "mysqli_field_seek",
        "mysqli_field_tell",
        "mysqli_free_result",
        "mysqli_get_connection_stats",
        "mysqli_get_client_stats",
        "mysqli_get_charset",
        "mysqli_get_client_info",
        "mysqli_get_client_version",
        "mysqli_get_links_stats",
        "mysqli_get_host_info",
        "mysqli_get_proto_info",
        "mysqli_get_server_info",
        "mysqli_get_server_version",
        "mysqli_get_warnings",
        "mysqli_init",
        "mysqli_info",
        "mysqli_insert_id",
        "mysqli_kill",
        "mysqli_more_results",
        "mysqli_multi_query",
        "mysqli_next_result",
        "mysqli_num_fields",
        "mysqli_num_rows",
        "mysqli_options",
        "mysqli_ping",
        "mysqli_poll",
        "mysqli_prepare",
        "mysqli_report",
        "mysqli_query",
        "mysqli_real_connect",
        "mysqli_real_escape_string",
        "mysqli_real_query",
        "mysqli_reap_async_query",
        "mysqli_release_savepoint",
        "mysqli_rollback",
        "mysqli_savepoint",
        "mysqli_select_db",
        "mysqli_set_charset",
        "mysqli_stmt_affected_rows",
        "mysqli_stmt_attr_get",
        "mysqli_stmt_attr_set",
        "mysqli_stmt_bind_param",
        "mysqli_stmt_bind_result",
        "mysqli_stmt_close",
        "mysqli_stmt_data_seek",
        "mysqli_stmt_errno",
        "mysqli_stmt_error",
        "mysqli_stmt_error_list",
        "mysqli_stmt_fetch",
        "mysqli_stmt_field_count",
        "mysqli_stmt_free_result",
        "mysqli_stmt_get_result",
        "mysqli_stmt_get_warnings",
        "mysqli_stmt_init",
        "mysqli_stmt_insert_id",
        "mysqli_stmt_more_results",
        "mysqli_stmt_next_result",
        "mysqli_stmt_num_rows",
        "mysqli_stmt_param_count",
        "mysqli_stmt_prepare",
        "mysqli_stmt_reset",
        "mysqli_stmt_result_metadata",
        "mysqli_stmt_send_long_data",
        "mysqli_stmt_store_result",
        "mysqli_stmt_sqlstate",
        "mysqli_sqlstate",
        "mysqli_ssl_set",
        "mysqli_stat",
        "mysqli_store_result",
        "mysqli_thread_id",
        "mysqli_thread_safe",
        "mysqli_use_result",
        "mysqli_warning_count",
        "mysqli_refresh",
        "mysqli_escape_string",
        "mysqli_set_opt",
        "pcntl_fork",
        "pcntl_waitpid",
        "pcntl_wait",
        "pcntl_signal",
        "pcntl_signal_get_handler",
        "pcntl_signal_dispatch",
        "pcntl_wifexited",
        "pcntl_wifstopped",
        "pcntl_wifsignaled",
        "pcntl_wexitstatus",
        "pcntl_wtermsig",
        "pcntl_wstopsig",
        "pcntl_exec",
        "pcntl_alarm",
        "pcntl_get_last_error",
        "pcntl_errno",
        "pcntl_strerror",
        "pcntl_getpriority",
        "pcntl_setpriority",
        "pcntl_sigprocmask",
        "pcntl_sigwaitinfo",
        "pcntl_sigtimedwait",
        "pcntl_wifcontinued",
        "pcntl_async_signals",
        "spl_classes",
        "spl_autoload",
        "spl_autoload_extensions",
        "spl_autoload_register",
        "spl_autoload_unregister",
        "spl_autoload_functions",
        "spl_autoload_call",
        "class_parents",
        "class_implements",
        "class_uses",
        "spl_object_hash",
        "iterator_to_array",
        "iterator_count",
        "iterator_apply",
        "pdo_drivers",
        "posix_kill",
        "posix_getpid",
        "posix_getppid",
        "posix_getuid",
        "posix_setuid",
        "posix_geteuid",
        "posix_seteuid",
        "posix_getgid",
        "posix_setgid",
        "posix_getegid",
        "posix_setegid",
        "posix_getgroups",
        "posix_getlogin",
        "posix_getpgrp",
        "posix_setsid",
        "posix_setpgid",
        "posix_getpgid",
        "posix_getsid",
        "posix_uname",
        "posix_times",
        "posix_ctermid",
        "posix_ttyname",
        "posix_isatty",
        "posix_getcwd",
        "posix_mkfifo",
        "posix_mknod",
        "posix_access",
        "posix_getgrnam",
        "posix_getgrgid",
        "posix_getpwnam",
        "posix_getpwuid",
        "posix_getrlimit",
        "posix_setrlimit",
        "posix_get_last_error",
        "posix_errno",
        "posix_strerror",
        "posix_initgroups",
        "session_name",
        "session_module_name",
        "session_save_path",
        "session_id",
        "session_create_id",
        "session_regenerate_id",
        "session_decode",
        "session_encode",
        "session_start",
        "session_destroy",
        "session_unset",
        "session_gc",
        "session_set_save_handler",
        "session_cache_limiter",
        "session_cache_expire",
        "session_set_cookie_params",
        "session_get_cookie_params",
        "session_write_close",
        "session_abort",
        "session_reset",
        "session_status",
        "session_register_shutdown",
        "session_commit",
        "shmop_open",
        "shmop_read",
        "shmop_close",
        "shmop_size",
        "shmop_write",
        "shmop_delete",
        "simplexml_load_file",
        "simplexml_load_string",
        "simplexml_import_dom",
        "use_soap_error_handler",
        "is_soap_fault",
        "socket_select",
        "socket_create",
        "socket_create_listen",
        "socket_create_pair",
        "socket_accept",
        "socket_set_nonblock",
        "socket_set_block",
        "socket_listen",
        "socket_close",
        "socket_write",
        "socket_read",
        "socket_getsockname",
        "socket_getpeername",
        "socket_connect",
        "socket_strerror",
        "socket_bind",
        "socket_recv",
        "socket_send",
        "socket_recvfrom",
        "socket_sendto",
        "socket_get_option",
        "socket_set_option",
        "socket_shutdown",
        "socket_last_error",
        "socket_clear_error",
        "socket_import_stream",
        "socket_export_stream",
        "socket_sendmsg",
        "socket_recvmsg",
        "socket_cmsg_space",
        "socket_getopt",
        "socket_setopt",
        "constant",
        "bin2hex",
        "hex2bin",
        "sleep",
        "usleep",
        "time_nanosleep",
        "time_sleep_until",
        "strptime",
        "flush",
        "wordwrap",
        "htmlspecialchars",
        "htmlentities",
        "html_entity_decode",
        "htmlspecialchars_decode",
        "get_html_translation_table",
        "sha1",
        "sha1_file",
        "md5",
        "md5_file",
        "crc32",
        "iptcparse",
        "iptcembed",
        "getimagesize",
        "getimagesizefromstring",
        "image_type_to_mime_type",
        "image_type_to_extension",
        "phpinfo",
        "phpversion",
        "phpcredits",
        "php_sapi_name",
        "php_uname",
        "php_ini_scanned_files",
        "php_ini_loaded_file",
        "strnatcmp",
        "strnatcasecmp",
        "substr_count",
        "strspn",
        "strcspn",
        "strtok",
        "strtoupper",
        "strtolower",
        "strpos",
        "stripos",
        "strrpos",
        "strripos",
        "strrev",
        "hebrev",
        "hebrevc",
        "nl2br",
        "basename",
        "dirname",
        "pathinfo",
        "stripslashes",
        "stripcslashes",
        "strstr",
        "stristr",
        "strrchr",
        "str_shuffle",
        "str_word_count",
        "str_split",
        "strpbrk",
        "substr_compare",
        "strcoll",
        "money_format",
        "substr",
        "substr_replace",
        "quotemeta",
        "ucfirst",
        "lcfirst",
        "ucwords",
        "strtr",
        "addslashes",
        "addcslashes",
        "rtrim",
        "str_replace",
        "str_ireplace",
        "str_repeat",
        "count_chars",
        "chunk_split",
        "trim",
        "ltrim",
        "strip_tags",
        "similar_text",
        "explode",
        "implode",
        "join",
        "setlocale",
        "localeconv",
        "nl_langinfo",
        "soundex",
        "levenshtein",
        "chr",
        "ord",
        "parse_str",
        "str_getcsv",
        "str_pad",
        "chop",
        "strchr",
        "sprintf",
        "printf",
        "vprintf",
        "vsprintf",
        "fprintf",
        "vfprintf",
        "sscanf",
        "fscanf",
        "parse_url",
        "urlencode",
        "urldecode",
        "rawurlencode",
        "rawurldecode",
        "http_build_query",
        "readlink",
        "linkinfo",
        "symlink",
        "link",
        "unlink",
        "exec",
        "system",
        "escapeshellcmd",
        "escapeshellarg",
        "passthru",
        "shell_exec",
        "proc_open",
        "proc_close",
        "proc_terminate",
        "proc_get_status",
        "proc_nice",
        "rand",
        "srand",
        "getrandmax",
        "mt_rand",
        "mt_srand",
        "mt_getrandmax",
        "random_bytes",
        "random_int",
        "getservbyname",
        "getservbyport",
        "getprotobyname",
        "getprotobynumber",
        "getmyuid",
        "getmygid",
        "getmypid",
        "getmyinode",
        "getlastmod",
        "base64_decode",
        "base64_encode",
        "password_hash",
        "password_get_info",
        "password_needs_rehash",
        "password_verify",
        "convert_uuencode",
        "convert_uudecode",
        "abs",
        "ceil",
        "floor",
        "round",
        "sin",
        "cos",
        "tan",
        "asin",
        "acos",
        "atan",
        "atanh",
        "atan2",
        "sinh",
        "cosh",
        "tanh",
        "asinh",
        "acosh",
        "expm1",
        "log1p",
        "pi",
        "is_finite",
        "is_nan",
        "is_infinite",
        "pow",
        "exp",
        "log",
        "log10",
        "sqrt",
        "hypot",
        "deg2rad",
        "rad2deg",
        "bindec",
        "hexdec",
        "octdec",
        "decbin",
        "decoct",
        "dechex",
        "base_convert",
        "number_format",
        "fmod",
        "intdiv",
        "inet_ntop",
        "inet_pton",
        "ip2long",
        "long2ip",
        "getenv",
        "putenv",
        "getopt",
        "sys_getloadavg",
        "microtime",
        "gettimeofday",
        "getrusage",
        "uniqid",
        "quoted_printable_decode",
        "quoted_printable_encode",
        "convert_cyr_string",
        "get_current_user",
        "set_time_limit",
        "header_register_callback",
        "get_cfg_var",
        "get_magic_quotes_gpc",
        "get_magic_quotes_runtime",
        "error_log",
        "error_get_last",
        "error_clear_last",
        "call_user_func",
        "call_user_func_array",
        "forward_static_call",
        "forward_static_call_array",
        "serialize",
        "unserialize",
        "var_dump",
        "var_export",
        "debug_zval_dump",
        "print_r",
        "memory_get_usage",
        "memory_get_peak_usage",
        "register_shutdown_function",
        "register_tick_function",
        "unregister_tick_function",
        "highlight_file",
        "show_source",
        "highlight_string",
        "php_strip_whitespace",
        "ini_get",
        "ini_get_all",
        "ini_set",
        "ini_alter",
        "ini_restore",
        "get_include_path",
        "set_include_path",
        "restore_include_path",
        "setcookie",
        "setrawcookie",
        "header",
        "header_remove",
        "headers_sent",
        "headers_list",
        "http_response_code",
        "connection_aborted",
        "connection_status",
        "ignore_user_abort",
        "parse_ini_file",
        "parse_ini_string",
        "is_uploaded_file",
        "move_uploaded_file",
        "gethostbyaddr",
        "gethostbyname",
        "gethostbynamel",
        "gethostname",
        "dns_check_record",
        "checkdnsrr",
        "dns_get_mx",
        "getmxrr",
        "dns_get_record",
        "intval",
        "floatval",
        "doubleval",
        "strval",
        "boolval",
        "gettype",
        "settype",
        "is_null",
        "is_resource",
        "is_bool",
        "is_int",
        "is_float",
        "is_integer",
        "is_long",
        "is_double",
        "is_real",
        "is_numeric",
        "is_string",
        "is_array",
        "is_object",
        "is_scalar",
        "is_callable",
        "is_iterable",
        "pclose",
        "popen",
        "readfile",
        "rewind",
        "rmdir",
        "umask",
        "fclose",
        "feof",
        "fgetc",
        "fgets",
        "fgetss",
        "fread",
        "fopen",
        "fpassthru",
        "ftruncate",
        "fstat",
        "fseek",
        "ftell",
        "fflush",
        "fwrite",
        "fputs",
        "mkdir",
        "rename",
        "copy",
        "tempnam",
        "tmpfile",
        "file",
        "file_get_contents",
        "file_put_contents",
        "stream_select",
        "stream_context_create",
        "stream_context_set_params",
        "stream_context_get_params",
        "stream_context_set_option",
        "stream_context_get_options",
        "stream_context_get_default",
        "stream_context_set_default",
        "stream_filter_prepend",
        "stream_filter_append",
        "stream_filter_remove",
        "stream_socket_client",
        "stream_socket_server",
        "stream_socket_accept",
        "stream_socket_get_name",
        "stream_socket_recvfrom",
        "stream_socket_sendto",
        "stream_socket_enable_crypto",
        "stream_socket_shutdown",
        "stream_socket_pair",
        "stream_copy_to_stream",
        "stream_get_contents",
        "stream_supports_lock",
        "fgetcsv",
        "fputcsv",
        "flock",
        "get_meta_tags",
        "stream_set_read_buffer",
        "stream_set_write_buffer",
        "set_file_buffer",
        "stream_set_chunk_size",
        "stream_set_blocking",
        "socket_set_blocking",
        "stream_get_meta_data",
        "stream_get_line",
        "stream_wrapper_register",
        "stream_register_wrapper",
        "stream_wrapper_unregister",
        "stream_wrapper_restore",
        "stream_get_wrappers",
        "stream_get_transports",
        "stream_resolve_include_path",
        "stream_is_local",
        "get_headers",
        "stream_set_timeout",
        "socket_set_timeout",
        "socket_get_status",
        "realpath",
        "fnmatch",
        "fsockopen",
        "pfsockopen",
        "pack",
        "unpack",
        "get_browser",
        "crypt",
        "opendir",
        "closedir",
        "chdir",
        "getcwd",
        "rewinddir",
        "readdir",
        "dir",
        "scandir",
        "glob",
        "fileatime",
        "filectime",
        "filegroup",
        "fileinode",
        "filemtime",
        "fileowner",
        "fileperms",
        "filesize",
        "filetype",
        "file_exists",
        "is_writable",
        "is_writeable",
        "is_readable",
        "is_executable",
        "is_file",
        "is_dir",
        "is_link",
        "stat",
        "lstat",
        "chown",
        "chgrp",
        "lchown",
        "lchgrp",
        "chmod",
        "touch",
        "clearstatcache",
        "disk_total_space",
        "disk_free_space",
        "diskfreespace",
        "realpath_cache_size",
        "realpath_cache_get",
        "mail",
        "ezmlm_hash",
        "openlog",
        "syslog",
        "closelog",
        "lcg_value",
        "metaphone",
        "ob_start",
        "ob_flush",
        "ob_clean",
        "ob_end_flush",
        "ob_end_clean",
        "ob_get_flush",
        "ob_get_clean",
        "ob_get_length",
        "ob_get_level",
        "ob_get_status",
        "ob_get_contents",
        "ob_implicit_flush",
        "ob_list_handlers",
        "ksort",
        "krsort",
        "natsort",
        "natcasesort",
        "asort",
        "arsort",
        "sort",
        "rsort",
        "usort",
        "uasort",
        "uksort",
        "shuffle",
        "array_walk",
        "array_walk_recursive",
        "count",
        "end",
        "prev",
        "next",
        "reset",
        "current",
        "key",
        "min",
        "max",
        "in_array",
        "array_search",
        "extract",
        "compact",
        "array_fill",
        "array_fill_keys",
        "range",
        "array_multisort",
        "array_push",
        "array_pop",
        "array_shift",
        "array_unshift",
        "array_splice",
        "array_slice",
        "array_merge",
        "array_merge_recursive",
        "array_replace",
        "array_replace_recursive",
        "array_keys",
        "array_values",
        "array_count_values",
        "array_column",
        "array_reverse",
        "array_reduce",
        "array_pad",
        "array_flip",
        "array_change_key_case",
        "array_rand",
        "array_unique",
        "array_intersect",
        "array_intersect_key",
        "array_intersect_ukey",
        "array_uintersect",
        "array_intersect_assoc",
        "array_uintersect_assoc",
        "array_intersect_uassoc",
        "array_uintersect_uassoc",
        "array_diff",
        "array_diff_key",
        "array_diff_ukey",
        "array_udiff",
        "array_diff_assoc",
        "array_udiff_assoc",
        "array_diff_uassoc",
        "array_udiff_uassoc",
        "array_sum",
        "array_product",
        "array_filter",
        "array_map",
        "array_chunk",
        "array_combine",
        "array_key_exists",
        "pos",
        "sizeof",
        "key_exists",
        "assert",
        "assert_options",
        "version_compare",
        "ftok",
        "str_rot13",
        "stream_get_filters",
        "stream_filter_register",
        "stream_bucket_make_writeable",
        "stream_bucket_prepend",
        "stream_bucket_append",
        "stream_bucket_new",
        "output_add_rewrite_var",
        "output_reset_rewrite_vars",
        "sys_get_temp_dir",
        "sem_get",
        "sem_acquire",
        "sem_release",
        "sem_remove",
        "token_get_all",
        "token_name",
        "xml_parser_create",
        "xml_parser_create_ns",
        "xml_set_object",
        "xml_set_element_handler",
        "xml_set_character_data_handler",
        "xml_set_processing_instruction_handler",
        "xml_set_default_handler",
        "xml_set_unparsed_entity_decl_handler",
        "xml_set_notation_decl_handler",
        "xml_set_external_entity_ref_handler",
        "xml_set_start_namespace_decl_handler",
        "xml_set_end_namespace_decl_handler",
        "xml_parse",
        "xml_parse_into_struct",
        "xml_get_error_code",
        "xml_error_string",
        "xml_get_current_line_number",
        "xml_get_current_column_number",
        "xml_get_current_byte_index",
        "xml_parser_free",
        "xml_parser_set_option",
        "xml_parser_get_option",
        "utf8_encode",
        "utf8_decode",
        "xmlrpc_encode",
        "xmlrpc_decode",
        "xmlrpc_decode_request",
        "xmlrpc_encode_request",
        "xmlrpc_get_type",
        "xmlrpc_set_type",
        "xmlrpc_is_fault",
        "xmlrpc_server_create",
        "xmlrpc_server_destroy",
        "xmlrpc_server_register_method",
        "xmlrpc_server_call_method",
        "xmlrpc_parse_method_descriptions",
        "xmlrpc_server_add_introspection_data",
        "xmlrpc_server_register_introspection_callback",
        "xmlwriter_open_uri",
        "xmlwriter_open_memory",
        "xmlwriter_set_indent",
        "xmlwriter_set_indent_string",
        "xmlwriter_start_comment",
        "xmlwriter_end_comment",
        "xmlwriter_start_attribute",
        "xmlwriter_end_attribute",
        "xmlwriter_write_attribute",
        "xmlwriter_start_attribute_ns",
        "xmlwriter_write_attribute_ns",
        "xmlwriter_start_element",
        "xmlwriter_end_element",
        "xmlwriter_full_end_element",
        "xmlwriter_start_element_ns",
        "xmlwriter_write_element",
        "xmlwriter_write_element_ns",
        "xmlwriter_start_pi",
        "xmlwriter_end_pi",
        "xmlwriter_write_pi",
        "xmlwriter_start_cdata",
        "xmlwriter_end_cdata",
        "xmlwriter_write_cdata",
        "xmlwriter_text",
        "xmlwriter_write_raw",
        "xmlwriter_start_document",
        "xmlwriter_end_document",
        "xmlwriter_write_comment",
        "xmlwriter_start_dtd",
        "xmlwriter_end_dtd",
        "xmlwriter_write_dtd",
        "xmlwriter_start_dtd_element",
        "xmlwriter_end_dtd_element",
        "xmlwriter_write_dtd_element",
        "xmlwriter_start_dtd_attlist",
        "xmlwriter_end_dtd_attlist",
        "xmlwriter_write_dtd_attlist",
        "xmlwriter_start_dtd_entity",
        "xmlwriter_end_dtd_entity",
        "xmlwriter_write_dtd_entity",
        "xmlwriter_output_memory",
        "xmlwriter_flush",
        "zip_open",
        "zip_close",
        "zip_read",
        "zip_entry_open",
        "zip_entry_close",
        "zip_entry_read",
        "zip_entry_filesize",
        "zip_entry_name",
        "zip_entry_compressedsize",
        "zip_entry_compressionmethod",
        "sg_load",
        "sg_get_mac_addresses",
        "sg_get_const",
        "sg_get_machine_id",
        "sg_get_verification_id",
        "sg_load_file",
        "sg_encode_file",
        "sg_eval",
        "sg_encode_string",
        "sg_decode_string",
        "sg_loader_version",
        "dl",
        "cli_set_process_title",
        "cli_get_process_title",
				"get_mangled_object_vars",
				"gc_status",
				"openssl_get_cert_locations",
				"openssl_spki_new",
				"openssl_spki_verify",
				"openssl_spki_export",
				"openssl_spki_export_challenge",
				"openssl_pkey_free",
				"openssl_pkey_new",
				"openssl_pkey_export",
				"openssl_pkey_export_to_file",
				"openssl_pkey_get_private",
				"openssl_pkey_get_public",
				"openssl_pkey_get_details",
				"openssl_free_key",
				"openssl_get_privatekey",
				"openssl_get_publickey",
				"openssl_x509_read",
				"openssl_x509_free",
				"openssl_x509_parse",
				"openssl_x509_checkpurpose",
				"openssl_x509_check_private_key",
				"openssl_x509_verify",
				"openssl_x509_export",
				"openssl_x509_fingerprint",
				"openssl_x509_export_to_file",
				"openssl_pkcs12_export",
				"openssl_pkcs12_export_to_file",
				"openssl_pkcs12_read",
				"openssl_csr_new",
				"openssl_csr_export",
				"openssl_csr_export_to_file",
				"openssl_csr_sign",
				"openssl_csr_get_subject",
				"openssl_csr_get_public_key",
				"openssl_digest",
				"openssl_encrypt",
				"openssl_decrypt",
				"openssl_cipher_iv_length",
				"openssl_sign",
				"openssl_verify",
				"openssl_seal",
				"openssl_open",
				"openssl_pbkdf2",
				"openssl_pkcs7_verify",
				"openssl_pkcs7_decrypt",
				"openssl_pkcs7_sign",
				"openssl_pkcs7_encrypt",
				"openssl_pkcs7_read",
				"openssl_private_encrypt",
				"openssl_private_decrypt",
				"openssl_public_encrypt",
				"openssl_public_decrypt",
				"openssl_get_md_methods",
				"openssl_get_cipher_methods",
				"openssl_get_curve_names",
				"openssl_dh_compute_key",
				"openssl_pkey_derive",
				"openssl_random_pseudo_bytes",
				"openssl_error_string",
				"inflate_get_status",
				"inflate_get_read_len",
				"ftp_ssl_connect",
				"ftp_mlsd",
				"ftp_append",
				"imagecreatefromwebp",
				"imagecreatefrombmp",
				"imagecreatefromtga",
				"imagewebp",
				"imagebmp",
				"imageopenpolygon",
				"imagesetclip",
				"imagegetclip",
				"imageresolution",
				"hash_hmac_algos",
				"collator_create",
				"collator_compare",
				"collator_get_attribute",
				"collator_set_attribute",
				"collator_get_strength",
				"collator_set_strength",
				"collator_sort",
				"collator_sort_with_sort_keys",
				"collator_asort",
				"collator_get_locale",
				"collator_get_error_code",
				"collator_get_error_message",
				"collator_get_sort_key",
				"numfmt_create",
				"numfmt_format",
				"numfmt_parse",
				"numfmt_format_currency",
				"numfmt_parse_currency",
				"numfmt_set_attribute",
				"numfmt_get_attribute",
				"numfmt_set_text_attribute",
				"numfmt_get_text_attribute",
				"numfmt_set_symbol",
				"numfmt_get_symbol",
				"numfmt_set_pattern",
				"numfmt_get_pattern",
				"numfmt_get_locale",
				"numfmt_get_error_code",
				"numfmt_get_error_message",
				"normalizer_normalize",
				"normalizer_is_normalized",
				"locale_get_default",
				"locale_set_default",
				"locale_get_primary_language",
				"locale_get_script",
				"locale_get_region",
				"locale_get_keywords",
				"locale_get_display_script",
				"locale_get_display_region",
				"locale_get_display_name",
				"locale_get_display_language",
				"locale_get_display_variant",
				"locale_compose",
				"locale_parse",
				"locale_get_all_variants",
				"locale_filter_matches",
				"locale_canonicalize",
				"locale_lookup",
				"locale_accept_from_http",
				"msgfmt_create",
				"msgfmt_format",
				"msgfmt_format_message",
				"msgfmt_parse",
				"msgfmt_parse_message",
				"msgfmt_set_pattern",
				"msgfmt_get_pattern",
				"msgfmt_get_locale",
				"msgfmt_get_error_code",
				"msgfmt_get_error_message",
				"datefmt_create",
				"datefmt_get_datetype",
				"datefmt_get_timetype",
				"datefmt_get_calendar",
				"datefmt_get_calendar_object",
				"datefmt_set_calendar",
				"datefmt_get_locale",
				"datefmt_get_timezone_id",
				"datefmt_get_timezone",
				"datefmt_set_timezone",
				"datefmt_get_pattern",
				"datefmt_set_pattern",
				"datefmt_is_lenient",
				"datefmt_set_lenient",
				"datefmt_format",
				"datefmt_format_object",
				"datefmt_parse",
				"datefmt_localtime",
				"datefmt_get_error_code",
				"datefmt_get_error_message",
				"grapheme_strlen",
				"grapheme_strpos",
				"grapheme_stripos",
				"grapheme_strrpos",
				"grapheme_strripos",
				"grapheme_substr",
				"grapheme_strstr",
				"grapheme_stristr",
				"grapheme_extract",
				"idn_to_ascii",
				"idn_to_utf8",
				"resourcebundle_create",
				"resourcebundle_get",
				"resourcebundle_count",
				"resourcebundle_locales",
				"resourcebundle_get_error_code",
				"resourcebundle_get_error_message",
				"transliterator_create",
				"transliterator_create_from_rules",
				"transliterator_list_ids",
				"transliterator_create_inverse",
				"transliterator_transliterate",
				"transliterator_get_error_code",
				"transliterator_get_error_message",
				"intltz_create_time_zone",
				"intltz_from_date_time_zone",
				"intltz_create_default",
				"intltz_get_id",
				"intltz_get_gmt",
				"intltz_get_unknown",
				"intltz_create_enumeration",
				"intltz_count_equivalent_ids",
				"intltz_create_time_zone_id_enumeration",
				"intltz_get_canonical_id",
				"intltz_get_region",
				"intltz_get_tz_data_version",
				"intltz_get_equivalent_id",
				"intltz_use_daylight_time",
				"intltz_get_offset",
				"intltz_get_raw_offset",
				"intltz_has_same_rules",
				"intltz_get_display_name",
				"intltz_get_dst_savings",
				"intltz_to_date_time_zone",
				"intltz_get_error_code",
				"intltz_get_error_message",
				"intlcal_create_instance",
				"intlcal_get_keyword_values_for_locale",
				"intlcal_get_now",
				"intlcal_get_available_locales",
				"intlcal_get",
				"intlcal_get_time",
				"intlcal_set_time",
				"intlcal_add",
				"intlcal_set_time_zone",
				"intlcal_after",
				"intlcal_before",
				"intlcal_set",
				"intlcal_roll",
				"intlcal_clear",
				"intlcal_field_difference",
				"intlcal_get_actual_maximum",
				"intlcal_get_actual_minimum",
				"intlcal_get_day_of_week_type",
				"intlcal_get_first_day_of_week",
				"intlcal_get_greatest_minimum",
				"intlcal_get_least_maximum",
				"intlcal_get_locale",
				"intlcal_get_maximum",
				"intlcal_get_minimal_days_in_first_week",
				"intlcal_get_minimum",
				"intlcal_get_time_zone",
				"intlcal_get_type",
				"intlcal_get_weekend_transition",
				"intlcal_in_daylight_time",
				"intlcal_is_equivalent_to",
				"intlcal_is_lenient",
				"intlcal_is_set",
				"intlcal_is_weekend",
				"intlcal_set_first_day_of_week",
				"intlcal_set_lenient",
				"intlcal_set_minimal_days_in_first_week",
				"intlcal_equals",
				"intlcal_from_date_time",
				"intlcal_to_date_time",
				"intlcal_get_repeated_wall_time_option",
				"intlcal_get_skipped_wall_time_option",
				"intlcal_set_repeated_wall_time_option",
				"intlcal_set_skipped_wall_time_option",
				"intlcal_get_error_code",
				"intlcal_get_error_message",
				"intlgregcal_create_instance",
				"intlgregcal_set_gregorian_change",
				"intlgregcal_get_gregorian_change",
				"intlgregcal_is_leap_year",
				"intl_get_error_code",
				"intl_get_error_message",
				"intl_is_failure",
				"intl_error_name",
				"mb_str_split",
				"mb_ord",
				"mb_chr",
				"mb_scrub",
				"pcntl_unshare",
				"spl_object_id",
				"socket_addrinfo_lookup",
				"socket_addrinfo_connect",
				"socket_addrinfo_bind",
				"socket_addrinfo_explain",
				"sodium_crypto_aead_aes256gcm_is_available",
				"sodium_crypto_aead_aes256gcm_decrypt",
				"sodium_crypto_aead_aes256gcm_encrypt",
				"sodium_crypto_aead_aes256gcm_keygen",
				"sodium_crypto_aead_chacha20poly1305_decrypt",
				"sodium_crypto_aead_chacha20poly1305_encrypt",
				"sodium_crypto_aead_chacha20poly1305_keygen",
				"sodium_crypto_aead_chacha20poly1305_ietf_decrypt",
				"sodium_crypto_aead_chacha20poly1305_ietf_encrypt",
				"sodium_crypto_aead_chacha20poly1305_ietf_keygen",
				"sodium_crypto_aead_xchacha20poly1305_ietf_decrypt",
				"sodium_crypto_aead_xchacha20poly1305_ietf_keygen",
				"sodium_crypto_aead_xchacha20poly1305_ietf_encrypt",
				"sodium_crypto_auth",
				"sodium_crypto_auth_keygen",
				"sodium_crypto_auth_verify",
				"sodium_crypto_box",
				"sodium_crypto_box_keypair",
				"sodium_crypto_box_seed_keypair",
				"sodium_crypto_box_keypair_from_secretkey_and_publickey",
				"sodium_crypto_box_open",
				"sodium_crypto_box_publickey",
				"sodium_crypto_box_publickey_from_secretkey",
				"sodium_crypto_box_seal",
				"sodium_crypto_box_seal_open",
				"sodium_crypto_box_secretkey",
				"sodium_crypto_kx_keypair",
				"sodium_crypto_kx_publickey",
				"sodium_crypto_kx_secretkey",
				"sodium_crypto_kx_seed_keypair",
				"sodium_crypto_kx_client_session_keys",
				"sodium_crypto_kx_server_session_keys",
				"sodium_crypto_generichash",
				"sodium_crypto_generichash_keygen",
				"sodium_crypto_generichash_init",
				"sodium_crypto_generichash_update",
				"sodium_crypto_generichash_final",
				"sodium_crypto_kdf_derive_from_key",
				"sodium_crypto_kdf_keygen",
				"sodium_crypto_pwhash",
				"sodium_crypto_pwhash_str",
				"sodium_crypto_pwhash_str_verify",
				"sodium_crypto_pwhash_str_needs_rehash",
				"sodium_crypto_pwhash_scryptsalsa208sha256",
				"sodium_crypto_pwhash_scryptsalsa208sha256_str",
				"sodium_crypto_pwhash_scryptsalsa208sha256_str_verify",
				"sodium_crypto_scalarmult",
				"sodium_crypto_secretbox",
				"sodium_crypto_secretbox_keygen",
				"sodium_crypto_secretbox_open",
				"sodium_crypto_secretstream_xchacha20poly1305_keygen",
				"sodium_crypto_secretstream_xchacha20poly1305_init_push",
				"sodium_crypto_secretstream_xchacha20poly1305_push",
				"sodium_crypto_secretstream_xchacha20poly1305_init_pull",
				"sodium_crypto_secretstream_xchacha20poly1305_pull",
				"sodium_crypto_secretstream_xchacha20poly1305_rekey",
				"sodium_crypto_shorthash",
				"sodium_crypto_shorthash_keygen",
				"sodium_crypto_sign",
				"sodium_crypto_sign_detached",
				"sodium_crypto_sign_ed25519_pk_to_curve25519",
				"sodium_crypto_sign_ed25519_sk_to_curve25519",
				"sodium_crypto_sign_keypair",
				"sodium_crypto_sign_keypair_from_secretkey_and_publickey",
				"sodium_crypto_sign_open",
				"sodium_crypto_sign_publickey",
				"sodium_crypto_sign_secretkey",
				"sodium_crypto_sign_publickey_from_secretkey",
				"sodium_crypto_sign_seed_keypair",
				"sodium_crypto_sign_verify_detached",
				"sodium_crypto_stream",
				"sodium_crypto_stream_keygen",
				"sodium_crypto_stream_xor",
				"sodium_add",
				"sodium_compare",
				"sodium_increment",
				"sodium_memcmp",
				"sodium_memzero",
				"sodium_pad",
				"sodium_unpad",
				"sodium_bin2hex",
				"sodium_hex2bin",
				"sodium_bin2base64",
				"sodium_base642bin",
				"sodium_crypto_scalarmult_base",
				"password_algos",
				"hrtime",
				"net_get_interfaces",
				"is_countable",
				"stream_isatty",
				"array_key_first",
				"array_key_last",
    };
    //php_printf(sizeof(php_inside_func));
    for(i2 = 0; i2 < 1721; ++i2){
        char str2[99]; 
        sprintf(str2, "%s", php_inside_func[i2]); 
        if(strcmp(str3, str2) == 0){
            return 1;
        }
    }
    return 0;
}

static void _dump_function_row_table(const HashTable *function_table, int level)
{
    zend_function *func;
    ZEND_HASH_FOREACH_PTR(function_table, func) {
        //if (level > 0) {
        //    php_printf("%*c", level, ' ');
        //}
        zend_string *function_name = func->common.function_name;

        if (function_name) {
            //php_printf("%s()", ZSTR_VAL(function_name));
            char strrr[99]; 
            sprintf(strrr, "%s", ZSTR_VAL(function_name)); 
            if( _in_SYS_Func_array(strrr) == 1 ){
                //php_printf("%s()", ZSTR_VAL(function_name));
                //php_printf("Skiped\n");
            }
            else
            {
                php_printf("Function: \n%s()\n", ZSTR_VAL(function_name));
                //php_printf("\n\"%s\",", ZSTR_VAL(function_name));
                vld_dump_oparray(&func->op_array);
                php_printf("---------------------------------\n");
            }
        }
        //vld_dump_oparray(&func->op_array);
        //ulop_dump_oparray_header(&func->op_array);
        //for (ii = 0; ii < &func->op_array->last; ii++) {
        //   ulop_dump_opline(&func->op_array->opcodes[ii], ii);
        //}
        //ulop_dump_oparray_footer(&func->op_array);

    } ZEND_HASH_FOREACH_END();
    return;
}

static void _dump_class_table(const HashTable *class_table, int level)
{
    zend_class_entry *ce;
    ZEND_HASH_FOREACH_PTR(class_table, ce) {
        if (level > 0) {
            //php_printf("%*c", level, ' ');
        }
        zend_string *class_name = ce->name;
        if (class_name) {
        	char strr[99]; 
        	sprintf(strr, "%s", ZSTR_VAL(class_name)); 
            if( _in_SYS_array(strr) == 1 ){
                //php_printf("%s::", ZSTR_VAL(class_name));
	            //php_printf("Skiped\n");
	        }
	        else
	        {
	        	//php_printf("\"%s\",\n", ZSTR_VAL(class_name));
	        	php_printf("%s::", ZSTR_VAL(class_name));
	            php_printf("\n");
	            _dump_function_table(&ce->function_table, 4);
	            _dump_properties_info(&ce->properties_info, 4);
	            _dump_constants_table(&ce->constants_table, 4);
	        }
        } else {
            php_printf("**unknown class**::");
        }
    } ZEND_HASH_FOREACH_END();                   
    return;
}

void _dump_properties_info(const HashTable *properties_info, int level)
{
    zend_property_info *prop_info;
    zend_string *key;
    zval *zv;

    ZEND_HASH_FOREACH_STR_KEY_VAL(properties_info, key, zv) {
        prop_info = Z_PTR_P(zv);
        zend_string *prop_name = prop_info->name;
        if (level > 0) {
            php_printf("%*c", level, ' ');
        }
        if (prop_name) {
            php_printf("%s", ZSTR_VAL(key));
        } else {
            php_printf("**unknown property**");
        }
        php_printf("\n");
    } ZEND_HASH_FOREACH_END();

    return;
}

void _dump_constants_table(const HashTable *constants_table, int level)
{
    zend_string *key;
    zval *zv;

    ZEND_HASH_FOREACH_STR_KEY_VAL(constants_table, key, zv) {
        if (level > 0) {
            php_printf("%*c", level, ' ');
        }
        if (key) {
            php_printf("%s: ", ZSTR_VAL(key));
            php_var_dump(zv, 1);
        } else {
            php_printf("**unknown property**\n");
        }
    } ZEND_HASH_FOREACH_END();

    return;
}

static char executed_filename[256];
static int execute_count;

/* {{{
 *    This function provides a hook for execution */
static void vld_execute_ex(zend_execute_data *execute_data)
{
	if (VLD_G(sg_decode))
	{
		if (strlen(executed_filename) == 0)
		{
			if (strlen(zend_get_executed_filename()) > 255)
			{
				php_printf("Warning: Filename is longer than 255 chars. Try renaming.\n");
			}
			else
			{
				strncpy(executed_filename, zend_get_executed_filename(), sizeof(executed_filename)-1);
				php_printf("Decoding %s. Includes will not be decoded.\n", executed_filename);
			}
		}

		if (execute_count == 1)
		{ 
			vld_dump_oparray(&execute_data->func->op_array);
			_dump_function_row_table(EG(function_table), 4);
			php_printf("======================================================\n");
			_dump_class_table(EG(class_table), 0);
		}

	}

	execute_count++;

  return old_execute_ex(execute_data TSRMLS_DC);
}
/* }}} */
