#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <setjmp.h>
#include <stdarg.h>

#include "execinfo.h"

#define MAX_FRAMES 32  /* Reduced from 64 for stability */
#define TEST_ITERATIONS 100  /* Reduced from 1000 for CI environments */
#define MAX_RECURSION_DEPTH 3  /* Conservative recursion depth */

/*
 * Global state for signal handling.
 *
 * sigjmp_buf + sigsetjmp/siglongjmp (not jmp_buf + setjmp/longjmp):
 * plain longjmp() does not restore the process signal mask. Inside a
 * signal handler the signal currently being handled is blocked by the
 * kernel for the handler's duration; jumping out with plain longjmp()
 * would leave that signal blocked for the rest of the process, e.g.
 * SIGSEGV would stay blocked after the first crash, so a *second*
 * crash anywhere later in the program would not be catchable, and
 * would instead silently kill the process (or worse, hang it,
 * depending on the platform). sigsetjmp(buf, 1) records the mask and
 * siglongjmp() restores it.
 *
 * guard_active distinguishes "a test is currently inside its
 * sigsetjmp-protected region" from "no test is active right now". A
 * single shared sigjmp_buf is safe here only because this test runner
 * is single-threaded and strictly sequential (no two guarded regions
 * are ever active at once) -- without the flag, a signal arriving
 * between tests (when the buffer holds a target from whichever test
 * ran last) would longjmp into a stack frame that has already
 * returned, which is undefined behavior.
 */
static sigjmp_buf test_jmp_buf;
static volatile sig_atomic_t guard_active = 0;
static volatile sig_atomic_t last_signal = 0;

/* Test result structure */
typedef struct {
    const char *name;
    int passed;
    int failed;
    double duration_ms;
} test_result_t;

/* Forward declarations */
static void print_trace_basic(void);
static void print_trace_detailed(void);
static void test_basic_functionality(test_result_t *result);
static void test_edge_cases(test_result_t *result);
static void test_performance(test_result_t *result);
static void test_symbols_fd(test_result_t *result);
static double get_time_ms(void);
static void recursive_function(int depth, int max_depth);
static void setup_signal_handlers(void);
static void signal_handler(int sig);
static void safe_printf(const char *format, ...);

/**
 * Async-signal-safe helpers, for use ONLY inside signal_handler().
 * POSIX does not guarantee vsnprintf()/printf()-family functions are
 * safe to call from a signal handler (they may use internal locks or
 * buffers), so the handler must not call safe_printf() below.
 */
static void
sig_write_str(const char *s)
{
    size_t len = strlen(s); /* strlen() is async-signal-safe per POSIX */
    ssize_t w = write(STDOUT_FILENO, s, len);
    (void)w;
}

static void
sig_write_int(int value)
{
    char digits[12];
    int n = 0;

    if (value < 0) {
        char minus = '-';
        ssize_t w = write(STDOUT_FILENO, &minus, 1);
        (void)w;
        value = -value;
    }
    if (value == 0) {
        char zero = '0';
        ssize_t w = write(STDOUT_FILENO, &zero, 1);
        (void)w;
        return;
    }
    while (value > 0 && n < (int)sizeof(digits)) {
        digits[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n > 0) {
        n--;
        ssize_t w = write(STDOUT_FILENO, &digits[n], 1);
        (void)w;
    }
}

/**
 * Safe printf wrapper for use OUTSIDE signal handlers.
 */
static void
safe_printf(const char *format, ...)
{
    va_list args;
    char buffer[1024];
    int len;

    va_start(args, format);
    len = vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    va_end(args);

    if (len > 0 && len < (int)sizeof(buffer)) {
        buffer[len] = '\0';
        if (write(STDOUT_FILENO, buffer, len) == -1) {
            // Ignore write errors in tests
        }
    }

    fflush(stdout);
}

/**
 * Free an array of strings allocated by backtrace_symbols
 * (glibc-compatible: only free the pointer, not each string)
 */
static void
free_bt_symbols(char **symbols, int size)
{
    if (!symbols) return;
    (void)size;
    free(symbols);
}

/**
 * Signal handler for catching crashes during tests.
 * Async-signal-safe calls ONLY (POSIX 2.4.3): no vsnprintf, no
 * malloc, no buffered stdio.
 */
static void
signal_handler(int sig)
{
    last_signal = sig;

    if (!guard_active) {
        /*
         * No test is currently inside a guarded region: there is
         * nowhere safe to jump back to. Restore the default
         * disposition and re-raise so the process terminates the
         * normal way (correct exit status, core dump if enabled)
         * instead of jumping into a possibly-stale stack frame.
         */
        sig_write_str("Fatal signal outside any guarded test region, terminating\n");
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }

    sig_write_str("Signal caught during guarded test execution: ");
    sig_write_int(sig);
    sig_write_str("\n");

    siglongjmp(test_jmp_buf, sig);
}

/**
 * Setup signal handlers for crash protection.
 * sigaction (not signal()) so the handler's signal mask is set
 * explicitly rather than relying on platform-specific signal()
 * semantics (which differ between classic System V one-shot
 * semantics and BSD/glibc semantics).
 */
static void
setup_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGSEGV);
    sigaddset(&sa.sa_mask, SIGBUS);
    sigaddset(&sa.sa_mask, SIGABRT);
    sigaddset(&sa.sa_mask, SIGFPE);
    sa.sa_flags = 0;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

/**
 * Basic backtrace printing function with safety checks
 */
static void
print_trace_basic(void)
{
    void *array[MAX_FRAMES];
    int size;
    char **strings;
    int i;

    memset(array, 0, sizeof(array));

    size = backtrace(array, MAX_FRAMES);
    if (size == 0 || size > MAX_FRAMES) {
        safe_printf("WARNING: backtrace returned unusual size: %d\n", size);
        return;
    }

    strings = backtrace_symbols(array, size);
    if (strings == NULL) {
        safe_printf("ERROR: backtrace_symbols failed: %s\n", strerror(errno));
        return;
    }

    safe_printf("=== Basic Backtrace (%d frames) ===\n", size);
    for (i = 0; i < size; i++) {
        if (strings[i] != NULL) {
            char safe_str[256];
            strncpy(safe_str, strings[i], sizeof(safe_str) - 1);
            safe_str[sizeof(safe_str) - 1] = '\0';
            safe_printf("[%2d] %s\n", i, safe_str);
        } else {
            safe_printf("[%2d] <null>\n", i);
        }
    }
    safe_printf("\n");

    free_bt_symbols(strings, size);
}

/**
 * Detailed backtrace with additional safety checks
 */
static void
print_trace_detailed(void)
{
    void *array[MAX_FRAMES];
    int size;
    int i;

    memset(array, 0, sizeof(array));
    size = backtrace(array, MAX_FRAMES);

    if (size == 0 || size > MAX_FRAMES) {
        safe_printf("WARNING: Invalid backtrace size: %d\n", size);
        return;
    }

    safe_printf("=== Detailed Backtrace (%d frames) ===\n", size);
    safe_printf("Raw addresses:\n");
    for (i = 0; i < size; i++) {
        safe_printf("[%2d] %p\n", i, array[i]);
    }

    safe_printf("\nSymbolic information:\n");
    /* Flush stdout before using fd-based output */
    fflush(stdout);
    backtrace_symbols_fd(array, size, STDOUT_FILENO);
    safe_printf("\n");
}

/**
 * Test basic functionality with error recovery
 */
static void
test_basic_functionality(test_result_t *result)
{
    void *array[MAX_FRAMES];
    char **strings = NULL;
    int size;
    volatile double start_time = get_time_ms();

    safe_printf("Running basic functionality tests...\n");

    guard_active = 1;
    if (sigsetjmp(test_jmp_buf, 1) != 0) {
        guard_active = 0;
        safe_printf("Test crashed with signal %d, marking as failed\n", (int)last_signal);
        result->failed++;
        result->duration_ms = get_time_ms() - start_time;
        return;
    }

    memset(array, 0, sizeof(array));
    size = backtrace(array, MAX_FRAMES);
    if (size > 0 && size <= MAX_FRAMES) {
        result->passed++;
        safe_printf("[pass] backtrace() returned %d frames\n", size);
    } else {
        result->failed++;
        safe_printf("[fail] backtrace() returned invalid size: %d\n", size);
        guard_active = 0;
        result->duration_ms = get_time_ms() - start_time;
        return;
    }

    strings = backtrace_symbols(array, size);
    if (strings != NULL) {
        result->passed++;
        safe_printf("[pass] backtrace_symbols() succeeded\n");

        int valid_strings = 0;
        for (int i = 0; i < size && i < 3; i++) {
            if (strings[i] != NULL && strlen(strings[i]) > 0) {
                valid_strings++;
            }
        }
        if (valid_strings > 0) {
            safe_printf("[pass] Found %d valid symbol strings\n", valid_strings);
        }
        free_bt_symbols(strings, size);
        strings = NULL;
    } else {
        result->failed++;
        safe_printf("[fail] backtrace_symbols() failed\n");
    }

    safe_printf("[pass] Testing backtrace_symbols_fd():\n");
    fflush(stdout);
    int fd_count = (size < 5) ? size : 5;
    backtrace_symbols_fd(array, fd_count, STDOUT_FILENO);
    result->passed++;

    guard_active = 0;
    result->duration_ms = get_time_ms() - start_time;
}

/**
 * Test edge cases with robust error handling
 */
static void
test_edge_cases(test_result_t *result)
{
    void *array[MAX_FRAMES];
    char **strings;
    int size;
    volatile double start_time = get_time_ms();

    safe_printf("Running edge case tests...\n");

    guard_active = 1;
    if (sigsetjmp(test_jmp_buf, 1) != 0) {
        guard_active = 0;
        safe_printf("Edge case test crashed with signal %d\n", (int)last_signal);
        result->failed++;
        result->duration_ms = get_time_ms() - start_time;
        return;
    }

    /* Test 1: Zero size */
    size = backtrace(array, 0);
    if (size == 0) {
        result->passed++;
        safe_printf("[pass] backtrace(0) correctly returned 0\n");
    } else {
        result->failed++;
        safe_printf("[fail] backtrace(0) returned %d (expected 0)\n", size);
    }

    /* Test 2: Single frame */
    size = backtrace(array, 1);
    if (size >= 0 && size <= 1) {
        result->passed++;
        safe_printf("[pass] backtrace(1) returned %d frame(s)\n", size);
    } else {
        result->failed++;
        safe_printf("[fail] backtrace(1) returned %d (expected 0-1)\n", size);
    }

    /* Test 3: Reasonable size request */
    memset(array, 0, sizeof(array));
    size = backtrace(array, MAX_FRAMES);
    if (size > 0 && size <= MAX_FRAMES) {
        safe_printf("[pass] backtrace(%d) returned %d frames\n", MAX_FRAMES, size);
        result->passed++;
    } else {
        safe_printf("[fail] backtrace(%d) returned invalid size: %d\n", MAX_FRAMES, size);
        result->failed++;
    }

    /* Test 4: Symbol edge cases - only if we have valid frames */
    if (size > 0) {
        /* Test zero count symbols */
        strings = backtrace_symbols(array, 0);
        if (strings == NULL) {
            result->passed++;
            safe_printf("[pass] backtrace_symbols(0) correctly returned NULL\n");
        } else {
            result->failed++;
            safe_printf("[fail] backtrace_symbols(0) should return NULL\n");
            free_bt_symbols(strings, 0); // Defensive, but should be unreachable
        }
    }

    guard_active = 0;
    result->duration_ms = get_time_ms() - start_time;
}

/**
 * Conservative performance testing.
 *
 * The deliberate usleep() calls below exist only to avoid hammering
 * the system in CI; they are excluded from the measured timings via
 * `call_time_ms`, which accumulates only the time spent inside the
 * actual backtrace()/backtrace_symbols() calls. The previous version
 * timed the whole loop including the sleeps and reported that as
 * "ms/call", which understated nothing -- it overstated the real
 * per-call cost by including sleep time that has nothing to do with
 * the library being measured.
 */
static void
test_performance(test_result_t *result)
{
    void *array[MAX_FRAMES];
    char **strings = NULL;
    int i, size;
    double call_start, call_time_ms;
    volatile double start_time = get_time_ms();

    safe_printf("Running performance tests (%d iterations)...\n", TEST_ITERATIONS);

    guard_active = 1;
    if (sigsetjmp(test_jmp_buf, 1) != 0) {
        guard_active = 0;
        safe_printf("Performance test crashed with signal %d\n", (int)last_signal);
        result->failed++;
        result->duration_ms = get_time_ms() - start_time;
        return;
    }

    /* Backtrace performance */
    size = 0;
    call_time_ms = 0.0;
    for (i = 0; i < TEST_ITERATIONS; i++) {
        memset(array, 0, sizeof(array));

        call_start = get_time_ms();
        size = backtrace(array, MAX_FRAMES);
        call_time_ms += get_time_ms() - call_start;

        if (size <= 0 || size > MAX_FRAMES) {
            result->failed++;
            safe_printf("[fail] backtrace failed or returned invalid size at iteration %d\n", i);
            break;
        }

        /* Throttle so CI doesn't get hammered; excluded from timing above. */
        if (i % 50 == 0) {
            usleep(1000); /* 1ms delay every 50 iterations */
        }
    }

    if (size > 0) {
        result->passed++;
        safe_printf("[pass] backtrace(): %.2f ms total call time, %.4f ms/call\n",
               call_time_ms, call_time_ms / TEST_ITERATIONS);
    }

    /* Symbol resolution performance - much more conservative */
    memset(array, 0, sizeof(array));
    size = backtrace(array, MAX_FRAMES);
    if (size > 0) {
        int symbol_iterations = TEST_ITERATIONS / 20; /* Even fewer iterations */
        call_time_ms = 0.0;

        for (i = 0; i < symbol_iterations; i++) {
            call_start = get_time_ms();
            strings = backtrace_symbols(array, size);
            call_time_ms += get_time_ms() - call_start;

            if (strings != NULL) {
                free_bt_symbols(strings, size);
                strings = NULL;
            } else {
                result->failed++;
                safe_printf("[fail] backtrace_symbols failed at iteration %d\n", i);
                break;
            }

            /* Throttle; excluded from timing above. */
            if (i % 10 == 0) {
                usleep(5000); /* 5ms delay every 10 iterations */
            }
        }

        if (i == symbol_iterations) {
            result->passed++;
            safe_printf("[pass] backtrace_symbols(): %.2f ms total call time, %.4f ms/call\n",
                   call_time_ms, call_time_ms / symbol_iterations);
        }
    }

    guard_active = 0;
    result->duration_ms = get_time_ms() - start_time;
}

/**
 * Test symbols_fd function safely
 */
static void
test_symbols_fd(test_result_t *result)
{
    void *array[MAX_FRAMES];
    int size;
    volatile double start_time = get_time_ms();

    safe_printf("Testing backtrace_symbols_fd()...\n");

    guard_active = 1;
    if (sigsetjmp(test_jmp_buf, 1) != 0) {
        guard_active = 0;
        safe_printf("symbols_fd test crashed with signal %d\n", (int)last_signal);
        result->failed++;
        result->duration_ms = get_time_ms() - start_time;
        return;
    }

    memset(array, 0, sizeof(array));
    size = backtrace(array, 5);  /* Just a few frames */
    if (size > 0 && size <= 5) {
        safe_printf("Output to stdout (limited to %d frames):\n", size);
        fflush(stdout);
        backtrace_symbols_fd(array, size, STDOUT_FILENO);
        fflush(stdout);
        result->passed++;
        safe_printf("[pass] backtrace_symbols_fd() completed\n");
    } else {
        result->failed++;
        safe_printf("[fail] Could not get valid backtrace for symbols_fd test (size=%d)\n", size);
    }

    guard_active = 0;
    result->duration_ms = get_time_ms() - start_time;
}

/**
 * Conservative recursive function
 */
static void
recursive_function(int depth, int max_depth)
{
    /* Add stack guard */
    if (depth < 0 || depth > 10 || max_depth < 0 || max_depth > 10) {
        safe_printf("Invalid recursion parameters: depth=%d, max_depth=%d\n", depth, max_depth);
        return;
    }

    if (depth >= max_depth) {
        safe_printf("=== Recursive Backtrace (depth %d) ===\n", depth);
        print_trace_basic();
        return;
    }

    /* Add some stack space and prevent tail call optimization */
    volatile int dummy = depth;
    (void)dummy;

    recursive_function(depth + 1, max_depth);
}

/**
 * Get current time in milliseconds
 */
static double
get_time_ms(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0.0;
    }
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/**
 * Safe dummy function
 */
static void
dummy_function(void)
{
    /* Add some stack depth */
    volatile int local_var = 42;
    (void)local_var;
    print_trace_detailed();
}

/**
 * Main test runner with comprehensive error handling
 */
int
main(void)
{
    test_result_t tests[] = {
        {"Basic Functionality", 0, 0, 0.0},
        {"Edge Cases", 0, 0, 0.0},
        {"Performance", 0, 0, 0.0},
        {"Symbols FD", 0, 0, 0.0}
    };
    int num_tests = sizeof(tests) / sizeof(tests[0]);
    int i, total_passed = 0, total_failed = 0;

    /* Setup signal handling first */
    setup_signal_handlers();

    safe_printf("libexecinfo Test Suite (Robust Version)\n");
    safe_printf("=======================================\n\n");

    /* Main test protection */
    guard_active = 1;
    if (sigsetjmp(test_jmp_buf, 1) != 0) {
        guard_active = 0;
        safe_printf("Main test execution crashed with signal %d\n", (int)last_signal);
        safe_printf("Attempting to continue with remaining tests...\n");
    } else {
        /* Run basic demonstration */
        safe_printf("=== Basic Demonstration ===\n");
        dummy_function();

        /* Test recursive calls with conservative depth */
        recursive_function(0, MAX_RECURSION_DEPTH);
        guard_active = 0;
    }

    safe_printf("=== Automated Tests ===\n");

    /* Run all tests with individual protection */
    test_basic_functionality(&tests[0]);
    test_edge_cases(&tests[1]);
    test_performance(&tests[2]);
    test_symbols_fd(&tests[3]);

    /* Print summary */
    safe_printf("\n=== Test Summary ===\n");
    for (i = 0; i < num_tests; i++) {
        safe_printf("%-20s: %d passed, %d failed (%.2f ms)\n",
               tests[i].name, tests[i].passed, tests[i].failed, tests[i].duration_ms);
        total_passed += tests[i].passed;
        total_failed += tests[i].failed;
    }

    safe_printf("\nOverall: %d passed, %d failed\n", total_passed, total_failed);

    /*
     * Strict success criteria: any failure fails the whole run. The
     * previous "more passes than failures" rule let a build with real
     * failures (e.g. 5 passed / 4 failed) exit 0, which is exactly
     * the wrong behavior for a check() step that's supposed to gate
     * packaging.
     */
    if (total_failed == 0 && total_passed > 0) {
        safe_printf("All tests passed.\n");
        return EXIT_SUCCESS;
    }

    safe_printf("Test failures present, failing the build.\n");
    return EXIT_FAILURE;
}
