// Basic string routines.  Not hardware optimized, but not shabby.

#include <inc/string.h>
#include <inc/malloc.h>

int
strlen(const char *s)
{
	int n;

	for (n=0; *s; s++)
		n++;
	return n;
}

char*
strcpy(char *dst, const char *src)
{
	char *ret;

	ret = dst;
	while ((*dst++ = *src++) != 0)
		;
	return ret;
}

char*
strncpy(char *dst, const char *src, size_t len)
{
	char *ret;
	
	ret = dst;
	while(len-- && (*dst++ = *src++) != 0)
		;
	return ret;
}

int
strcmp(const char *p, const char *q)
{
	while (*p && *p == *q)
		p++, q++;
	if ((unsigned char)*p < (unsigned char)*q)
		return -1;
	if ((unsigned char)*p > (unsigned char)*q)
		return 1;
	return 0;
}

int
strncmp(const char *p, const char *q, size_t len)
{
	while (len-- && *p && *p == *q)
		p++, q++;
	if(len == -1)
		return 0;
	if ((unsigned char)*p < (unsigned char)*q)
		return -1;
	if ((unsigned char)*p > (unsigned char)*q)
		return 1;
	return 0;
}

char*
strchr(const char *s, char c)
{
	for (; *s; s++)
		if (*s == c)
			return (char*) s;
	return 0;
}

char *
strstr(const char *big, const char *little)
{
	int length = strlen(little);
	for(; *big; big++)
		if(!strncmp(big, little, length))
			return (char *) big;
	return NULL;
}

#ifndef KUDOS_KERNEL
char *
strdup(const char *src)
{
	return memdup(src, strlen(src) + 1);
}
#endif

long
strtol(const char *s, char **endptr, int base)
{
	int neg = 0;
	long val = 0;

	// gobble initial whitespace
	while (*s == ' ' || *s == '\t')
		s++;

	// plus/minus sign
	if (*s == '+')
		s++;
	else if (*s == '-')
		s++, neg = 1;

	// hex or octal base prefix
	if ((base == 0 || base == 16) && (s[0] == '0' && s[1] == 'x'))
		s += 2, base = 16;
	else if (base == 0 && s[0] == '0')
		s++, base = 8;
	else if (base == 0)
		base = 10;

	// digits
	while (1) {
		int dig;

		if (*s >= '0' && *s <= '9')
			dig = *s - '0';
		else if (*s >= 'a' && *s <= 'z')
			dig = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'Z')
			dig = *s - 'A' + 10;
		else
			break;
		if (dig >= base)
			break;
		s++, val = (val * base) + dig;
		// we don't properly detect overflow!
	}

	if (endptr)
		*endptr = (char*)s;

	if (neg)
		return -val;
	else
		return val;
}

void *
memset(void *v, int c, size_t n)
{
	char *p;
	size_t m;

	p = v;
	for (m = 0; m < n; m++)
		*p++ = c;

	return v;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	const char *s;
	char *d;
	int m;

	s = src;
	d = dst;
	m = n;
	while (--m >= 0)
		*d++ = *s++;

	return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
	const char *s;
	char *d;
	int m;
	
	s = src;
	d = dst;
	m = n;
	if (d < s) {
		while (--m >= 0)
			*d++ = *s++;
	} else {
		s += m;
		d += m;
		while (--m >= 0)
			*--d = *--s;
	}
	
	return dst;
}

int
memcmp(const void *p, const void *q, size_t len)
{
	uint8_t * bp = (uint8_t *) p;
	uint8_t * bq = (uint8_t *) q;
	while(len-- > 0)
	{
		if(*bp != *bq)
			return (int) *bp - (int) *bq;
		bp++;
		bq++;
	}
	return 0;
}

#ifndef KUDOS_KERNEL
void *
memdup(const void *src, size_t len)
{
	void * copy = malloc(len);
	if(copy)
		memcpy(copy, src, len);
	return copy;
}
#endif

int
isnum(char c)
{
	if('0' <= c && c <= '9')
		return 1;
	else
		return 0;
}
