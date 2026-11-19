/* Newer glibc headers send strtol/sscanf to C23 versions that glibc 2.13 doesn't have. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#undef strtol
#undef sscanf
extern long strtol(const char *, char **, int) __asm__("strtol");
extern int vsscanf(const char *, const char *, va_list) __asm__("vsscanf");
__attribute__((visibility("hidden"))) long __isoc23_strtol(const char *s, char **e, int b) { return strtol(s, e, b); }
__attribute__((visibility("hidden"))) int __isoc23_sscanf(const char *s, const char *f, ...) {
    va_list ap; va_start(ap, f); int r = vsscanf(s, f, ap); va_end(ap); return r;
}
