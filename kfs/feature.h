#ifndef __KUDOS_KFS_FEATURE_H
#define __KUDOS_KFS_FEATURE_H

#include <inc/types.h>

struct feature;
typedef struct feature feature_t;

struct feature {
	uint32_t id:30, optional:1, warn:1;
	const char * description;
};

extern const feature_t KFS_feature_size;

#endif /* __KUDOS_KFS_FEATURE_H */
