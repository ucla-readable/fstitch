#ifndef KUDOS_INC_STRING_H
#define KUDOS_INC_STRING_H

#include <inc/types.h>

int		strlen(const char *s);
char *		strcpy(char *dest, const char *src);
char *		strncpy(char *dest, const char *src, size_t len);
int		strcmp(const char *s1, const char *s2);
int		strncmp(const char *s1, const char *s2, size_t len);
char *		strchr(const char *s, char c);
char *		strstr(const char *big, const char *little);
char *		strdup(const char *src);

long		strtol(const char *s, char **endptr, int base);

void *		memset(void *dest, int, size_t len);
void *		memcpy(void *dest, const void *src, size_t len);
void *		memmove(void *dest, const void *src, size_t len);
void *		memchr(const void *s, int c, size_t n);
void *		memdup(const void *src, size_t len);
int		memcmp(const void *p, const void *q, size_t len);

int      isnum(char c);

#endif /* not KUDOS_INC_STRING_H */
