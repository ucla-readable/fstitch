#include <inc/types.h>

struct feature {
	uint32_t id:30, optional:1, warn:1;
	const char * description;
};

typedef struct feature feature_t;
