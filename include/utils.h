#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

char *read_text_file(const char *path);
char *trim_whitespace(char *text);
char *strdup_safe(const char *src);
void *xmalloc(size_t size);

#endif
