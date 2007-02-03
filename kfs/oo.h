#ifndef __KUDOS_KFS_OO_H
#define __KUDOS_KFS_OO_H

#include <lib/types.h>
#include <lib/stdlib.h>

#include <kfs/magic.h>

#define OBJECT(interface) struct object uniform; int (*__destroy_type)(interface * object)
#define DECLARE(interface, type, method, args...) type (*_##method)(interface * object, ##args)

#define ASSIGN(object, module, method) (object)->_##method = module##_##method
#define OBJASSIGN(object, module, method) (object)->uniform._##method = module##_##method
#define DESTRUCTOR(object, module, method) (object)->uniform.__destroy = (int (*)(void *)) ((object)->__destroy_type = module##_##method)

#define CALL(object, method, args...) ((object)->_##method(object, ##args))
#define DESTROY(object) ((object)->uniform.__destroy(object))

#define OBJCALL(object, method, args...) ((object)->uniform._##method(object, ##args))
#define OBJFLAGS(object) (object)->uniform.flags
#define OBJMAGIC(object) (object)->uniform.magic
#define OBJLOCAL(object) (object)->uniform.local

#define OBJ_INIT(object, module, info) { \
	OBJLOCAL(object) = info; \
	OBJFLAGS(object) = 0; \
	OBJMAGIC(object) = 0; \
	OBJASSIGN(object, module, get_config); \
	OBJASSIGN(object, module, get_status); \
	DESTRUCTOR(object, module, destroy); \
}

/* values for OBJFLAGS */
#define OBJ_PERSISTENT 0x01

/* config and status levels */
#define CONFIG_VERBOSE 0
#define CONFIG_NORMAL 1
#define CONFIG_BRIEF 2
#define STATUS_VERBOSE 0
#define STATUS_NORMAL 1
#define STATUS_BRIEF 2

struct object {
	uint32_t flags, magic;
	DECLARE(void, int, get_config, int level, char * string, size_t length);
	DECLARE(void, int, get_status, int level, char * string, size_t length);
	int (*__destroy)(void * object);
	void * local;
};
typedef struct { struct object uniform; } object_t;

#endif /* __KUDOS_KFS_OO_H */
