#ifndef __KUDOS_KFS_OO_H
#define __KUDOS_KFS_OO_H

#define DECLARE(interface, type, method, args...) type (*_##method)(interface * object, ##args)
#define DESTRUCTOR(interface) int (*__destroy)(interface * object)

#define ASSIGN(object, module, method) (object)->_##method = module##_##method
#define ASSIGN_DESTROY(object, module, method) (object)->__destroy = module##_##method

#define CALL(object, method, args...) ((object)->_##method(object, ##args))
#define DESTROY(object) ((object)->__destroy(object))

// DESTROY_LOCAL is only for use outside of KFSD
#ifndef KFSD
#define DESTROY_LOCAL(object) free(object)
#else
#define DESTROY_LOCAL(object) static_assert(0)
#endif

#endif /* __KUDOS_KFS_OO_H */
