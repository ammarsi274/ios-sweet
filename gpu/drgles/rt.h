/*
 * The few C library calls drgles itself makes. Declared here instead of
 * including system headers: the bench builds without a sysroot (TWRP's
 * bionic has no headers on this PC) and iOS's libSystem has the same
 * signatures for these.
 */
#ifndef DRGLES_RT_H
#define DRGLES_RT_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strrchr(const char *, int);
char *strchr(const char *, int);
void *memmove(void *, const void *, size_t);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);

#endif
