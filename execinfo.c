#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "execinfo.h"
#include "stacktraverse.h"

#define MAX_STACK_BUFFER 4096

/*
 * Look up symbol information for ADDR via dladdr() and format it the
 * way glibc's backtrace_symbols() does: "<addr> <name+offset> at <file>",
 * falling back to a bare "<addr>" when no symbol is found.
 *
 * Returns a malloc'd, exactly-sized, NUL-terminated string, or NULL on
 * allocation failure. The caller owns the returned pointer.
 *
 * This replaces the previous approach of hand-computing the formatted
 * length with a digit-counting macro (D10). That macro assumed the
 * offset was always non-negative; dladdr() can return a symbol whose
 * address is *higher* than the queried pointer (e.g. for PLT stubs),
 * producing a negative offset, log10() of which is NaN, which produced
 * a buffer too small for the real snprintf() output and corrupted the
 * heap. snprintf(NULL, 0, ...) asks the C library to report the exact
 * length it would have written, which is correct by construction for
 * any offset, any symbol name length, and any file path length, with
 * no fixed-size temp buffer to overflow or silently truncate into.
 */
static char *
format_frame(void *addr)
{
    Dl_info info;
    char *out;
    int n;
    int have_info = addr && (dladdr(addr, &info) != 0);

    if (have_info) {
        const char *sname = info.dli_sname ? info.dli_sname : "???";
        void *saddr = info.dli_saddr ? info.dli_saddr : addr;
        ptrdiff_t offset = (char *)addr - (char *)saddr;

        n = snprintf(NULL, 0, "%p <%s+%td> at %s",
                     addr, sname, offset, info.dli_fname);
        if (n < 0)
            return NULL;

        out = malloc((size_t)n + 1);
        if (out == NULL)
            return NULL;

        snprintf(out, (size_t)n + 1, "%p <%s+%td> at %s",
                 addr, sname, offset, info.dli_fname);
        return out;
    }

    n = snprintf(NULL, 0, "%p", addr);
    if (n < 0)
        return NULL;

    out = malloc((size_t)n + 1);
    if (out == NULL)
        return NULL;

    snprintf(out, (size_t)n + 1, "%p", addr);
    return out;
}

int
backtrace(void **buffer, int size)
{
    int i;
    if (size <= 0)
        return 0;

    for (i = 0; i < size; i++) {
        void *addr = getreturnaddr(i);
        if (addr == NULL)
            break;
        buffer[i] = addr;
    }
    return i;
}

char **
backtrace_symbols(void *const *buffer, int size)
{
    char **formatted;
    char **rval;
    size_t *lens;
    size_t total_len = 0;
    char *cur;
    int i;

    if (size <= 0)
        return NULL;

    formatted = calloc((size_t)size, sizeof(char *));
    if (formatted == NULL)
        return NULL;

    lens = malloc((size_t)size * sizeof(size_t));
    if (lens == NULL) {
        free(formatted);
        return NULL;
    }

    for (i = 0; i < size; i++) {
        formatted[i] = format_frame(buffer[i]);
        if (formatted[i] == NULL) {
            /* Roll back everything formatted so far before giving up. */
            for (int j = 0; j < i; j++)
                free(formatted[j]);
            free(formatted);
            free(lens);
            return NULL;
        }
        lens[i] = strlen(formatted[i]) + 1; /* + NUL */
        total_len += lens[i];
    }

    /* Single allocation: an array of `size` char* followed by the
       packed string data, matching glibc's backtrace_symbols() layout
       so callers can free() the returned array with one call. */
    rval = malloc((size_t)size * sizeof(char *) + total_len);
    if (rval == NULL) {
        for (i = 0; i < size; i++)
            free(formatted[i]);
        free(formatted);
        free(lens);
        return NULL;
    }

    cur = (char *)(rval + size);
    for (i = 0; i < size; i++) {
        memcpy(cur, formatted[i], lens[i]);
        rval[i] = cur;
        cur += lens[i];
        free(formatted[i]);
    }

    free(formatted);
    free(lens);
    return rval;
}

/*
 * Unlike backtrace_symbols(), this function avoids malloc() for the
 * common case (short, stack-buffer-sized lines) so it remains usable
 * in low-memory situations. It still calls dladdr() and snprintf(),
 * neither of which POSIX guarantees to be async-signal-safe, so despite
 * appearances this is NOT strictly safe to call from inside a signal
 * handler on every platform -- see the note in execinfo.h.
 */
void
backtrace_symbols_fd(void *const *buffer, int size, int fd)
{
    int i, len;
    char *buf;
    char stack_buf[MAX_STACK_BUFFER];
    Dl_info info;
    ptrdiff_t offset = 0;
    ssize_t written;
    const char *sname = NULL;
    void *saddr = NULL;
    int have_info;

    if (size <= 0 || fd < 0)
        return;

    for (i = 0; i < size; i++) {
        have_info = buffer[i] && (dladdr(buffer[i], &info) != 0);

        if (have_info) {
            sname = info.dli_sname ? info.dli_sname : "???";
            saddr = info.dli_saddr ? info.dli_saddr : buffer[i];
            offset = (char *)buffer[i] - (char *)saddr;
            len = snprintf(NULL, 0, "%p <%s+%td> at %s\n",
                            buffer[i], sname, offset, info.dli_fname);
        } else {
            len = snprintf(NULL, 0, "%p\n", buffer[i]);
        }

        if (len < 0)
            continue; /* formatting error: skip this frame, keep going */

        if ((size_t)len < sizeof(stack_buf)) {
            buf = stack_buf;
        } else {
            buf = malloc((size_t)len + 1);
            if (buf == NULL)
                continue; /* OOM: skip this frame rather than abort the trace */
        }

        if (have_info) {
            snprintf(buf, (size_t)len + 1, "%p <%s+%td> at %s\n",
                     buffer[i], sname, offset, info.dli_fname);
        } else {
            snprintf(buf, (size_t)len + 1, "%p\n", buffer[i]);
        }

        written = write(fd, buf, (size_t)len);
        (void)written; /* best-effort; nothing useful to do on a write() failure here */

        if (buf != stack_buf)
            free(buf);
    }
}
