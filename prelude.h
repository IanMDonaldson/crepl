#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <inttypes.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>

/* Provided by the crepl binary (linked with -rdynamic).  Lets user code that
 * calls exit() unwind back into the REPL instead of killing the session. */
extern void __crepl_trap_exit(int) __attribute__((noreturn));
#undef exit
#define exit(c) __crepl_trap_exit((c))

static void __crepl_pr_str(const char *s)
{
    if (!s) { fputs("NULL", stdout); return; }
    fputc('"', stdout);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\' || c == '"') { fputc('\\', stdout); fputc((int)c, stdout); }
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c < 32 || c == 127) printf("\\x%02x", c);
        else fputc((int)c, stdout);
    }
    fputc('"', stdout);
}
static void __crepl_pr_bool(_Bool v) { fputs(v ? "true" : "false", stdout); }
static void __crepl_pr_char(char v)
{
    if (v >= 32 && v < 127) printf("'%c' (%d)", v, (int)v);
    else printf("'\\x%02x' (%d)", (unsigned char)v, (int)v);
}
static void __crepl_pr_schar(signed char v)        { printf("%d", (int)v); }
static void __crepl_pr_uchar(unsigned char v)      { printf("%u", (unsigned)v); }
static void __crepl_pr_short(short v)              { printf("%d", (int)v); }
static void __crepl_pr_ushort(unsigned short v)    { printf("%u", (unsigned)v); }
static void __crepl_pr_int(int v)                  { printf("%d", v); }
static void __crepl_pr_uint(unsigned v)            { printf("%u", v); }
static void __crepl_pr_long(long v)                { printf("%ld", v); }
static void __crepl_pr_ulong(unsigned long v)      { printf("%lu", v); }
static void __crepl_pr_llong(long long v)          { printf("%lld", v); }
static void __crepl_pr_ullong(unsigned long long v){ printf("%llu", v); }
static void __crepl_pr_float(float v)              { printf("%g", (double)v); }
static void __crepl_pr_double(double v)            { printf("%g", v); }
static void __crepl_pr_ldouble(long double v)      { printf("%Lg", v); }
static void __crepl_pr_ptr(const void *p)
{
    if (!p) fputs("NULL", stdout); else printf("%p", p);
}
static void __crepl_pr_bytes(const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t i, lim = n < 64 ? n : 64;
    printf("<%zu bytes:", n);
    for (i = 0; i < lim; i++) printf(" %02x", b[i]);
    if (n > lim) fputs(" ...", stdout);
    fputc('>', stdout);
}

#define __CREPL_SHOW(v) _Generic((v), \
    _Bool: __crepl_pr_bool, \
    char: __crepl_pr_char, \
    signed char: __crepl_pr_schar, \
    unsigned char: __crepl_pr_uchar, \
    short: __crepl_pr_short, \
    unsigned short: __crepl_pr_ushort, \
    int: __crepl_pr_int, \
    unsigned int: __crepl_pr_uint, \
    long: __crepl_pr_long, \
    unsigned long: __crepl_pr_ulong, \
    long long: __crepl_pr_llong, \
    unsigned long long: __crepl_pr_ullong, \
    float: __crepl_pr_float, \
    double: __crepl_pr_double, \
    long double: __crepl_pr_ldouble, \
    char *: __crepl_pr_str, \
    const char *: __crepl_pr_str, \
    default: __crepl_pr_ptr \
)(v)
