#ifndef HAKU_STDLIB_H
#define HAKU_STDLIB_H

#include <haku/types.h>

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);

int atoi(const char *text);

__declspec(noreturn) void exit(int status);

#endif
