#ifndef __KUDOS_KFS_OO_H
#define __KUDOS_KFS_OO_H

#include <kfs/magic.h>

#define OBJECT(interface) struct object uniform; int (*__destroy_type)(interface * object)
#define DECLARE(interface, type, method, args...) type (*_##method)(interface * object, ##args)

#define ASSIGN(object, module, method) (object)->_##method = module##_##method
#define OBJASSIGN(object, module, method) (object)->uniform._##method = module##_##method
#define DESTRUCTOR(object, module, method) (object)->uniform.__destroy = (int (*)(void *)) ((object)->__destroy_type = module##_##method)

#define CALL(object, method, args...) ((object)->_##method(object, ##args))
#define DESTROY(object) ((object)->uniform.__destroy(object))

#define OBJMAGIC(object) (object)->uniform.magic

#define OBJ_INIT(object, module) { \
	OBJMAGIC(object) = 0; \
	DESTRUCTOR(object, module, destroy); \
}

/* config and status levels */
#define CONFIG_VERBOSE 0
#define CONFIG_NORMAL 1
#define CONFIG_BRIEF 2
#define STATUS_VERBOSE 0
#define STATUS_NORMAL 1
#define STATUS_BRIEF 2

struct object {
	uint32_t magic;
	int (*__destroy)(void * object);
};
typedef struct { struct object uniform; } object_t;

#endif /* __KUDOS_KFS_OO_H */
