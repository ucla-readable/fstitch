#ifndef __KUDOS_KFS_OO_H
#define __KUDOS_KFS_OO_H

#define OBJECT(interface) object_t uniform; int (*__destroy_type)(interface * object)
#define DECLARE(interface, type, method, args...) type (*_##method)(interface * object, ##args)

#define ASSIGN(object, module, method) (object)->_##method = module##_##method
#define OBJASSIGN(object, module, method) (object)->uniform._##method = module##_##method
#define DESTRUCTOR(object, module, method) (object)->uniform.__destroy = (int (*)(void *)) ((object)->__destroy_type = module##_##method)

#define CALL(object, method, args...) ((object)->_##method(object, ##args))
#define DESTROY(object) ((object)->uniform.__destroy(object))

#define OBJCALL(object, method, args...) ((object)->uniform._##method(object, ##args))
#define OBJFLAGS(object) (object)->uniform.flags
#define OBJMAGIC(object) (object)->uniform.magic

struct object {
	uint32_t flags, magic;
	DECLARE(void, int, get_status, int level, char * string, int length);
	int (*__destroy)(void * object);
};
typedef struct object object_t;

// DESTROY_LOCAL is only for use outside of KFSD
#ifndef KFSD
extern void delete_obj(uint32_t id);
#define DESTROY_LOCAL(object) do { delete_obj((uint32_t) object->instance); free(object); } while(0)
#else
#define DESTROY_LOCAL(object) static_assert(0)
#endif

#endif /* __KUDOS_KFS_OO_H */
