#include <inc/types.h>

struct bdesc;
struct chmetadesc;

struct chdesc {
	struct bdesc * block;
	enum {BIT, BYTE} type;
	union {
		struct {
			uint32_t offset;
			uint32_t xor;
		} bit;
		struct {
			uint32_t offset;
			uint32_t length;
			uint8_t * olddata;
			uint8_t * newdata;
		} byte;
	} data;
	struct chmetadesc * dependencies;
	struct chmetadesc * dependents;
};

struct chmetadesc {
	struct chdesc * desc;
	struct chmetadesc * next;
};

typedef struct chdesc chdesc_t;
typedef struct chmetadesc chmetadesc_t;
