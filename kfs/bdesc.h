#include <inc/types.h>

struct BD;

struct bdesc {
	struct BD * bd;
	uint32_t number, refs;
	uint16_t offset, length;
	uint8_t * data;
};

typedef struct bdesc bdesc_t;
