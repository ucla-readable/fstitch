#ifndef __KUDOS_KFS_PLATFORM_H
#define __KUDOS_KFS_PLATFORM_H

#ifdef __KERNEL__

#include <linux/version.h>
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 18)
#include <linux/config.h>
#endif

#include <linux/ctype.h>
#include <linux/fcntl.h>

#define printf(format, ...) printk(KERN_ERR format, ## __VA_ARGS__)
/* assume that fprintf is only used for stderr */
#define fprintf(stream, format, ...) printk(KERN_EMERG format, ## __VA_ARGS__)

#include <lib/stdlib.h>
#include <linux/string.h>

static __inline char * strdup(const char * s)
{
	size_t len = strlen(s) + 1;
	char * c = malloc(len);
	if(!c)
		return NULL;
	strcpy(c, s);
	return c;
}

static __inline int strcasecmp(const char * s1, const char * s2)
{
	while(tolower(*s1) == tolower(*s2++))
		if(*s1++ == '\0')
			return 0;
	return tolower(*s1) - tolower(*--s2);
}

#include <linux/errno.h>
#include <lib/assert.h>
#include <lib/types.h>

#elif defined(UNIXUSER)

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdint.h>

#endif

#endif /* __KUDOS_KFS_PLATFORM_H */
