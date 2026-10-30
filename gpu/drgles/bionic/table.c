/*
 * Every symbol the Nokia's GPU libraries import from Android's system
 * libraries, mapped to drgles. Two kinds:
 *   ours   the bionic layer's implementation (b_*)
 *   host   functions whose ABI is the same in bionic and in the host's C
 *          library (Darwin's on iOS): string/memory, number parsing,
 *          ctype, math, zlib, struct tm time functions, malloc family.
 *          They are bound directly. (errno set by these lands in the
 *          host's errno, not the driver's; none of them is checked for it
 *          in the measured runs.)
 * Import list: research/vendor-adreno/survey/imports-all.txt (llvm-nm of
 * the vendor_b libraries).
 */
#include "bionic.h"

/* host C library */
extern int memcmp(const void *, const void *, size_t);

extern void *memchr(const void *, int, size_t);
extern size_t strnlen(const char *, size_t);
extern char *strstr(const char *, const char *);
extern int strncasecmp(const char *, const char *, size_t);
extern char *strtok_r(char *, const char *, char **);
extern size_t strlcpy(char *, const char *, size_t);
extern size_t strlcat(char *, const char *, size_t);
extern int atoi(const char *);
extern double atof(const char *);
extern long strtol(const char *, char **, int);
extern long long strtoll(const char *, char **, int);
extern unsigned long strtoul(const char *, char **, int);
extern unsigned long long strtoull(const char *, char **, int);
extern double strtod(const char *, char **);
extern int tolower(int), toupper(int), isalnum(int), isalpha(int), islower(int), isprint(int), isspace(int), isupper(int);
extern void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
extern int rand(void);
extern void srand(unsigned);
extern void *malloc(size_t), *calloc(size_t, size_t), *realloc(void *, size_t);
extern void free(void *);
extern void *localtime_r(const long *, void *);
extern long mktime(void *);
extern double difftime(long, long);
extern double acos(double), asin(double), atan(double), atan2(double, double), cos(double), cosh(double), exp(double),
	exp2(double), fmod(double, double), log(double), log10(double), modf(double, double *), pow(double, double),
	sin(double), sinh(double), tan(double), tanh(double);
extern float acosf(float), asinf(float), atanf(float), atan2f(float, float), cosf(float), coshf(float), exp2f(float),
	expf(float), fmodf(float, float), ldexpf(float, int), logf(float), powf(float, float), sinf(float), sinhf(float),
	tanf(float), tanhf(float);
extern int compress(void *, unsigned long *, const void *, unsigned long);
extern int uncompress(void *, unsigned long *, const void *, unsigned long);

#define D(name) void name(void);
/* ours, declared loosely: only their addresses are taken here */
D(b___errno) D(b_pthread_mutex_lock) D(b_pthread_mutex_trylock) D(b_pthread_mutex_unlock) D(b_pthread_mutex_init)
D(b_pthread_mutex_destroy) D(b_pthread_mutexattr_init) D(b_pthread_mutexattr_destroy) D(b_pthread_mutexattr_settype)
D(b_pthread_mutexattr_setpshared) D(b_pthread_cond_init) D(b_pthread_cond_destroy) D(b_pthread_cond_wait)
D(b_pthread_cond_timedwait) D(b_pthread_cond_signal) D(b_pthread_cond_broadcast) D(b_pthread_rwlock_init)
D(b_pthread_rwlock_destroy) D(b_pthread_rwlock_rdlock) D(b_pthread_rwlock_wrlock) D(b_pthread_rwlock_unlock)
D(b_sem_init) D(b_sem_destroy) D(b_sem_post) D(b_sem_wait) D(b_sem_timedwait) D(b_sem_timedwait_monotonic_np)
D(b_pthread_key_create) D(b_pthread_key_delete) D(b_pthread_getspecific) D(b_pthread_setspecific)
D(b_pthread_create) D(b_pthread_join) D(b_pthread_detach) D(b_pthread_self) D(b_pthread_equal) D(b_pthread_exit)
D(b_gettid) D(b_getpid) D(b_sched_setaffinity)
D(b_fopen) D(b_open_memstream) D(b_fclose) D(b_fflush) D(b_fread) D(b___fread_chk) D(b_fwrite) D(b___fwrite_chk)
D(b_fputs) D(b_fputc) D(b___fgets_chk) D(b_getline) D(b_fseeko) D(b_ftello) D(b_vsnprintf) D(b___vsnprintf_chk)
D(b_vfprintf) D(b_perror)
D(b___open_2) D(b_close) D(b_read) D(b___read_chk) D(b_write) D(b___write_chk) D(b_lseek) D(b_fstat) D(b_stat)
D(b_access) D(b_poll) D(b_dup) D(b_mmap) D(b_munmap) D(b_msync)
D(b_dlopen) D(b_dlsym) D(b_dlerror) D(b_dlclose) D(b_dladdr) D(b_property_get) D(b_property_get_bool)
D(b_property_set) D(b___system_property_get) D(b_getenv) D(b_atrace_setup) D(b_atrace_begin_body)
D(b_atrace_end_body) D(b_atrace_async_begin_body) D(b_atrace_async_end_body) D(b_atrace_int_body)
D(b_atrace_get_enabled_tags) D(b_sysconf) D(b_getpagesize) D(b_getauxval) D(b_sysinfo) D(b_uname) D(b_getrusage)
D(b_mallinfo) D(b_strerror) D(b___gnu_strerror_r) D(b_mkdir) D(b_rmdir) D(b_unlink) D(b_chmod) D(b_mkstemp)
D(b_system) D(b___getcwd_chk) D(b___umask_chk) D(b_isatty) D(b_getlogin) D(b_clock_gettime) D(b_gettimeofday)
D(b_usleep) D(b_abort) D(b_exit) D(b_raise) D(b___stack_chk_fail) D(b___cxa_atexit) D(b___cxa_finalize)
D(b___register_atfork) D(b___strlen_chk) D(b___strlcpy_chk) D(b___strlcat_chk) D(b___memcpy_chk) D(b___memset_chk)
D(b___strrchr_chk) D(b_memalign) D(b_sync_wait) D(b_sync_merge) D(b_native_handle_create) D(b_native_handle_delete)
D(b_native_handle_close) D(b_native_handle_clone) D(b_hw_get_module) D(b_basename) D(b_dirname)
D(b_new) D(b_new_nothrow) D(b_delete) D(b___cxa_pure_virtual) D(b___cxa_guard_acquire) D(b___cxa_guard_release)
D(b___cxa_guard_abort) D(b_str_grow_by) D(b_str_ctor_copy) D(b_str_ctor_sub) D(b_str_dtor) D(b_str_assign)
D(b_str_operator_eq) D(b_str_append_n) D(b_str_append) D(b_str_insert) D(b_str_insert_char) D(b_str_erase)
D(b_str_resize) D(b_str_reserve) D(b_str_push_back) D(b_str_copy) D(b_str_find_char) D(b_str_rfind_char)
D(b_str_compare_n) D(b_str_compare) D(b_str_plus) D(b_swc_get_deleter) D(b_swc_release_weak) D(b_swc_dtor)
D(b_throw_length_error) D(b_throw_out_of_range) D(b_sort_u32) D(b_sort_long) D(b_sort_ulong)
D(b_cxx_stream_stop) D(b_cxx_locale_stop) D(b_next_prime)
/* vararg.S */
D(drgv_printf) D(drgv_fprintf) D(drgv_sscanf) D(drgv_open) D(drgv_ioctl) D(drgv_fcntl) D(drgv_prctl)
D(drgv_android_log_print) D(drgv_android_log_assert) D(drgv_snprintf) D(drgv_sprintf) D(drg_setjmp) D(drg_longjmp)

extern struct bfile *b_stdin, *b_stdout, *b_stderr;
extern const char *b___progname;
extern uint8_t b_atrace_is_ready;
extern uint64_t b_atrace_enabled_tags;
extern char b_std_nothrow;
extern uint64_t b_cxx_zero_object[64];

#define S "_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE"
#define KS "_ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE"
#define IS "_ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE"
#define OS "_ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE"
#define IOS "_ZNSt3__114basic_iostreamIcNS_11char_traitsIcEEE"
#define SB "_ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE"

static const struct {
	const char *name;
	void *addr;
} tab[] = {
	/* ---- host: same ABI ---- */
	{ "memcpy", memcpy }, { "memmove", memmove }, { "memset", memset }, { "memcmp", memcmp }, { "memchr", memchr },
	{ "strcmp", strcmp }, { "strncmp", strncmp }, { "strlen", strlen }, { "strnlen", strnlen }, { "strchr", strchr },
	{ "strrchr", strrchr }, { "strstr", strstr }, { "strncasecmp", strncasecmp }, { "strtok_r", strtok_r },
	{ "strlcpy", strlcpy }, { "strlcat", strlcat }, { "atoi", atoi }, { "atof", atof }, { "strtol", strtol },
	{ "strtoll", strtoll }, { "strtoul", strtoul }, { "strtoull", strtoull }, { "strtod", strtod },
	{ "tolower", tolower }, { "toupper", toupper }, { "isalnum", isalnum }, { "isalpha", isalpha },
	{ "islower", islower }, { "isprint", isprint }, { "isspace", isspace }, { "isupper", isupper },
	{ "qsort", qsort }, { "rand", rand }, { "srand", srand }, { "malloc", malloc }, { "calloc", calloc },
	{ "realloc", realloc }, { "free", free }, { "localtime_r", localtime_r }, { "mktime", mktime },
	{ "difftime", difftime },
	{ "acos", acos }, { "acosf", acosf }, { "asin", asin }, { "asinf", asinf }, { "atan", atan }, { "atanf", atanf },
	{ "atan2", atan2 }, { "atan2f", atan2f }, { "cos", cos }, { "cosf", cosf }, { "cosh", cosh }, { "coshf", coshf },
	{ "exp", exp }, { "expf", expf }, { "exp2", exp2 }, { "exp2f", exp2f }, { "fmod", fmod }, { "fmodf", fmodf },
	{ "ldexpf", ldexpf }, { "log", log }, { "logf", logf }, { "log10", log10 }, { "modf", modf }, { "pow", pow },
	{ "powf", powf }, { "sin", sin }, { "sinf", sinf }, { "sinh", sinh }, { "sinhf", sinhf }, { "tan", tan },
	{ "tanf", tanf }, { "tanh", tanh }, { "tanhf", tanhf },
	{ "compress", compress }, { "uncompress", uncompress },

	/* ---- ours: threads ---- */
	{ "__errno", b___errno }, { "pthread_mutex_lock", b_pthread_mutex_lock },
	{ "pthread_mutex_trylock", b_pthread_mutex_trylock }, { "pthread_mutex_unlock", b_pthread_mutex_unlock },
	{ "pthread_mutex_init", b_pthread_mutex_init }, { "pthread_mutex_destroy", b_pthread_mutex_destroy },
	{ "pthread_mutexattr_init", b_pthread_mutexattr_init }, { "pthread_mutexattr_destroy", b_pthread_mutexattr_destroy },
	{ "pthread_mutexattr_settype", b_pthread_mutexattr_settype },
	{ "pthread_mutexattr_setpshared", b_pthread_mutexattr_setpshared },
	{ "pthread_cond_init", b_pthread_cond_init }, { "pthread_cond_destroy", b_pthread_cond_destroy },
	{ "pthread_cond_wait", b_pthread_cond_wait }, { "pthread_cond_timedwait", b_pthread_cond_timedwait },
	{ "pthread_cond_signal", b_pthread_cond_signal }, { "pthread_cond_broadcast", b_pthread_cond_broadcast },
	{ "pthread_rwlock_init", b_pthread_rwlock_init }, { "pthread_rwlock_destroy", b_pthread_rwlock_destroy },
	{ "pthread_rwlock_rdlock", b_pthread_rwlock_rdlock }, { "pthread_rwlock_wrlock", b_pthread_rwlock_wrlock },
	{ "pthread_rwlock_unlock", b_pthread_rwlock_unlock }, { "sem_init", b_sem_init }, { "sem_destroy", b_sem_destroy },
	{ "sem_post", b_sem_post }, { "sem_wait", b_sem_wait }, { "sem_timedwait", b_sem_timedwait },
	{ "sem_timedwait_monotonic_np", b_sem_timedwait_monotonic_np }, { "pthread_key_create", b_pthread_key_create },
	{ "pthread_key_delete", b_pthread_key_delete }, { "pthread_getspecific", b_pthread_getspecific },
	{ "pthread_setspecific", b_pthread_setspecific }, { "pthread_create", b_pthread_create },
	{ "pthread_join", b_pthread_join }, { "pthread_detach", b_pthread_detach }, { "pthread_self", b_pthread_self },
	{ "pthread_equal", b_pthread_equal }, { "pthread_exit", b_pthread_exit }, { "gettid", b_gettid },
	{ "getpid", b_getpid }, { "sched_setaffinity", b_sched_setaffinity },

	/* ---- ours: stdio ---- */
	{ "stdin", &b_stdin }, { "stdout", &b_stdout }, { "stderr", &b_stderr }, { "fopen", b_fopen },
	{ "open_memstream", b_open_memstream }, { "fclose", b_fclose }, { "fflush", b_fflush }, { "fread", b_fread },
	{ "__fread_chk", b___fread_chk }, { "fwrite", b_fwrite }, { "__fwrite_chk", b___fwrite_chk },
	{ "fputs", b_fputs }, { "fputc", b_fputc }, { "__fgets_chk", b___fgets_chk }, { "getline", b_getline },
	{ "fseek", b_fseeko }, { "fseeko", b_fseeko }, { "ftell", b_ftello }, { "ftello", b_ftello },
	{ "vsnprintf", b_vsnprintf }, { "__vsnprintf_chk", b___vsnprintf_chk }, { "vfprintf", b_vfprintf },
	{ "perror", b_perror }, { "printf", drgv_printf }, { "fprintf", drgv_fprintf }, { "sscanf", drgv_sscanf },
	{ "snprintf", drgv_snprintf }, { "sprintf", drgv_sprintf },

	/* ---- ours: files, devices, memory ---- */
	{ "open", drgv_open }, { "__open_2", b___open_2 }, { "close", b_close }, { "read", b_read },
	{ "__read_chk", b___read_chk }, { "write", b_write }, { "__write_chk", b___write_chk }, { "lseek", b_lseek },
	{ "fstat", b_fstat }, { "stat", b_stat }, { "access", b_access }, { "poll", b_poll }, { "dup", b_dup },
	{ "fcntl", drgv_fcntl }, { "ioctl", drgv_ioctl }, { "mmap", b_mmap }, { "mmap64", b_mmap },
	{ "munmap", b_munmap }, { "msync", b_msync },

	/* ---- ours: misc ---- */
	{ "dlopen", b_dlopen }, { "dlsym", b_dlsym }, { "dlerror", b_dlerror }, { "dlclose", b_dlclose },
	{ "dladdr", b_dladdr }, { "__android_log_print", drgv_android_log_print },
	{ "__android_log_assert", drgv_android_log_assert }, { "property_get", b_property_get },
	{ "property_get_bool", b_property_get_bool }, { "property_set", b_property_set },
	{ "__system_property_get", b___system_property_get }, { "getenv", b_getenv },
	{ "atrace_is_ready", &b_atrace_is_ready }, { "atrace_enabled_tags", &b_atrace_enabled_tags },
	{ "atrace_setup", b_atrace_setup }, { "atrace_begin_body", b_atrace_begin_body },
	{ "atrace_end_body", b_atrace_end_body }, { "atrace_async_begin_body", b_atrace_async_begin_body },
	{ "atrace_async_end_body", b_atrace_async_end_body }, { "atrace_int_body", b_atrace_int_body },
	{ "atrace_get_enabled_tags", b_atrace_get_enabled_tags }, { "sysconf", b_sysconf },
	{ "getpagesize", b_getpagesize }, { "getauxval", b_getauxval }, { "sysinfo", b_sysinfo }, { "uname", b_uname },
	{ "getrusage", b_getrusage }, { "mallinfo", b_mallinfo }, { "strerror", b_strerror },
	{ "__gnu_strerror_r", b___gnu_strerror_r }, { "mkdir", b_mkdir }, { "rmdir", b_rmdir }, { "unlink", b_unlink },
	{ "chmod", b_chmod }, { "mkstemp", b_mkstemp }, { "system", b_system }, { "__getcwd_chk", b___getcwd_chk },
	{ "__umask_chk", b___umask_chk }, { "isatty", b_isatty }, { "getlogin", b_getlogin },
	{ "clock_gettime", b_clock_gettime }, { "gettimeofday", b_gettimeofday }, { "usleep", b_usleep },
	{ "abort", b_abort }, { "exit", b_exit }, { "raise", b_raise }, { "__stack_chk_fail", b___stack_chk_fail },
	{ "__progname", &b___progname }, { "prctl", drgv_prctl }, { "__cxa_atexit", b___cxa_atexit },
	{ "__cxa_finalize", b___cxa_finalize }, { "__register_atfork", b___register_atfork },
	{ "__strlen_chk", b___strlen_chk }, { "__strlcpy_chk", b___strlcpy_chk }, { "__strlcat_chk", b___strlcat_chk },
	{ "__memcpy_chk", b___memcpy_chk }, { "__memset_chk", b___memset_chk }, { "__strrchr_chk", b___strrchr_chk },
	{ "memalign", b_memalign }, { "setjmp", drg_setjmp }, { "longjmp", drg_longjmp }, { "sync_wait", b_sync_wait },
	{ "sync_merge", b_sync_merge }, { "native_handle_create", b_native_handle_create },
	{ "native_handle_delete", b_native_handle_delete }, { "native_handle_close", b_native_handle_close },
	{ "native_handle_clone", b_native_handle_clone }, { "hw_get_module", b_hw_get_module }, { "basename", b_basename }, { "dirname", b_dirname },

	/* ---- ours: C++ ---- */
	{ "_Znwm", b_new }, { "_Znam", b_new }, { "_ZdlPv", b_delete }, { "_ZdaPv", b_delete },
	{ "_ZnwmRKSt9nothrow_t", b_new_nothrow }, { "_ZnamRKSt9nothrow_t", b_new_nothrow },
	{ "_ZSt7nothrow", &b_std_nothrow }, { "__cxa_pure_virtual", b___cxa_pure_virtual },
	{ "__cxa_guard_acquire", b___cxa_guard_acquire }, { "__cxa_guard_release", b___cxa_guard_release },
	{ "__cxa_guard_abort", b___cxa_guard_abort },
	{ KS "4copyEPcmm", b_str_copy }, { KS "4findEcm", b_str_find_char }, { KS "5rfindEcm", b_str_rfind_char },
	{ KS "7compareEmmPKc", b_str_compare }, { KS "7compareEmmPKcm", b_str_compare_n },
	{ S "5eraseEmm", b_str_erase }, { S "6appendEPKc", b_str_append }, { S "6appendEPKcm", b_str_append_n },
	{ S "6assignEPKc", b_str_assign }, { S "6insertENS_11__wrap_iterIPKcEEc", b_str_insert_char },
	{ S "6insertEmPKc", b_str_insert }, { S "6resizeEmc", b_str_resize }, { S "7reserveEm", b_str_reserve },
	{ S "9__grow_byEmmmmmm", b_str_grow_by }, { S "9push_backEc", b_str_push_back },
	{ S "C1ERKS5_", b_str_ctor_copy }, { S "C1ERKS5_mmRKS4_", b_str_ctor_sub }, { S "D2Ev", b_str_dtor },
	{ S "aSERKS5_", b_str_operator_eq },
	{ "_ZNSt3__1plIcNS_11char_traitsIcEENS_9allocatorIcEEEENS_12basic_stringIT_T0_T1_EEPKS6_RKS9_", b_str_plus },
	{ "_ZNKSt3__119__shared_weak_count13__get_deleterERKSt9type_info", b_swc_get_deleter },
	{ "_ZNSt3__119__shared_weak_count14__release_weakEv", b_swc_release_weak },
	{ "_ZNSt3__119__shared_weak_countD2Ev", b_swc_dtor },
	{ "_ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv", b_throw_length_error },
	{ "_ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv", b_throw_out_of_range },
	{ "_ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv", b_throw_length_error },
	{ "_ZNSt3__16__sortIRNS_6__lessIjjEEPjEEvT0_S5_T_", b_sort_u32 },
	{ "_ZNSt3__16__sortIRNS_6__lessIllEEPlEEvT0_S5_T_", b_sort_long },
	{ "_ZNSt3__16__sortIRNS_6__lessImmEEPmEEvT0_S5_T_", b_sort_ulong },
	{ "_ZNSt3__112__next_primeEm", b_next_prime },
	{ "_ZNSt3__15mutex4lockEv", b_pthread_mutex_lock }, { "_ZNSt3__15mutex6unlockEv", b_pthread_mutex_unlock },
	{ "_ZNSt3__15mutexD1Ev", b_pthread_mutex_destroy },
	/* streams and locales: never called in the measured runs */
	{ "_ZNSt3__14coutE", b_cxx_zero_object }, { "_ZNSt3__15ctypeIcE2idE", b_cxx_zero_object + 40 },
	{ "_ZNSt3__17codecvtIcc9mbstate_tE2idE", b_cxx_zero_object + 48 },
	{ "_ZNKSt3__16locale9has_facetERNS0_2idE", b_cxx_locale_stop },
	{ "_ZNKSt3__16locale9use_facetERNS0_2idE", b_cxx_locale_stop }, { "_ZNKSt3__18ios_base6getlocEv", b_cxx_locale_stop },
	{ "_ZNSt3__16localeC1ERKS0_", b_cxx_locale_stop }, { "_ZNSt3__16localeD1Ev", b_cxx_locale_stop },
	{ IS "D0Ev", b_cxx_stream_stop }, { IS "D1Ev", b_cxx_stream_stop }, { IS "D2Ev", b_cxx_stream_stop },
	{ IS "rsERj", b_cxx_stream_stop }, { OS "3putEc", b_cxx_stream_stop }, { OS "5flushEv", b_cxx_stream_stop },
	{ OS "5writeEPKcl", b_cxx_stream_stop }, { OS "6sentryC1ERS3_", b_cxx_stream_stop },
	{ OS "6sentryD1Ev", b_cxx_stream_stop }, { OS "D0Ev", b_cxx_stream_stop }, { OS "D1Ev", b_cxx_stream_stop },
	{ OS "D2Ev", b_cxx_stream_stop }, { OS "lsEi", b_cxx_stream_stop }, { OS "lsEj", b_cxx_stream_stop },
	{ OS "lsEl", b_cxx_stream_stop }, { OS "lsEm", b_cxx_stream_stop }, { IOS "D0Ev", b_cxx_stream_stop },
	{ IOS "D1Ev", b_cxx_stream_stop }, { IOS "D2Ev", b_cxx_stream_stop }, { SB "4syncEv", b_cxx_stream_stop },
	{ SB "5imbueERKNS_6localeE", b_cxx_stream_stop }, { SB "5uflowEv", b_cxx_stream_stop },
	{ SB "6setbufEPcl", b_cxx_stream_stop }, { SB "6xsgetnEPcl", b_cxx_stream_stop },
	{ SB "6xsputnEPKcl", b_cxx_stream_stop }, { SB "9showmanycEv", b_cxx_stream_stop },
	{ SB "C2Ev", b_cxx_stream_stop }, { SB "D2Ev", b_cxx_stream_stop },
	{ "_ZNSt3__18ios_base4initEPv", b_cxx_stream_stop }, { "_ZNSt3__18ios_base5clearEj", b_cxx_stream_stop },
	{ "_ZNSt3__19basic_iosIcNS_11char_traitsIcEEED2Ev", b_cxx_stream_stop },
	{ "_ZThn16_NSt3__114basic_iostreamIcNS_11char_traitsIcEEED0Ev", b_cxx_stream_stop },
	{ "_ZThn16_NSt3__114basic_iostreamIcNS_11char_traitsIcEEED1Ev", b_cxx_stream_stop },
	{ "_ZTv0_n24_NSt3__113basic_istreamIcNS_11char_traitsIcEEED0Ev", b_cxx_stream_stop },
	{ "_ZTv0_n24_NSt3__113basic_istreamIcNS_11char_traitsIcEEED1Ev", b_cxx_stream_stop },
	{ "_ZTv0_n24_NSt3__113basic_ostreamIcNS_11char_traitsIcEEED0Ev", b_cxx_stream_stop },
	{ "_ZTv0_n24_NSt3__113basic_ostreamIcNS_11char_traitsIcEEED1Ev", b_cxx_stream_stop },
	{ "_ZTv0_n24_NSt3__114basic_iostreamIcNS_11char_traitsIcEEED0Ev", b_cxx_stream_stop },
	{ "_ZTv0_n24_NSt3__114basic_iostreamIcNS_11char_traitsIcEEED1Ev", b_cxx_stream_stop },
};

void *bionic_lookup(const char *name)
{
	for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
		if (!strcmp(tab[i].name, name))
			return tab[i].addr;
	return 0;
}
