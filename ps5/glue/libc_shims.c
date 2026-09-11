// A few libc functions the console's libc.prx does not export, each trivial to
// build on ones it does (vsnprintf, sinf, cosf).
// Compiled with -fno-builtin: otherwise the compiler turns sinf + cosf back
// into a call to sincosf, and sincosf calls itself forever.

#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

int sprintf(char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vsnprintf(str, 0x7FFFFFFF, format, args);
    va_end(args);
    return ret;
}

void sincosf(float x, float *s, float *c) {
    *s = sinf(x);
    *c = cosf(x);
}

// The BSD libc's assert() expands to __assert(func, file, line, expr), not
// glibc's __assert_fail.
void __assert(const char *func, const char *file, int line, const char *expr) {
    (void)func; (void)file; (void)line; (void)expr;
    abort();
}
