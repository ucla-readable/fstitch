#include <lib/assert.h>
#include <lib/kdprintf.h>
#include <lib/vector.h>

#include <kfs/cfs.h>
#include <kfs/lfs.h>
#include <kfs/bd.h>
#include <kfs/modman.h>
#include <kfs/destroy.h>

#define DESTROY_ALL_ENABLED 0


// Destroy all modules of type 'module' that have no users
#define DESTROY_ALL(module, type) \
static size_t destroy_all_##type(void) \
{ \
	module * mod; \
	modman_it_t it; \
	vector_t * mods; \
	size_t i; \
	size_t ndestroyed = 0; \
	int r; \
	\
	/* Load all mods of type 'module' into a vector since we'll modify the modman hash_map */ \
	mods = vector_create(); \
	assert(mods); \
	r = modman_it_init_##type(&it); \
	assert(!r); \
	while ((mod = modman_it_next_##type(&it))) \
	{ \
		r = vector_push_back(mods, mod); \
		assert(!r); \
	} \
	mod = NULL; \
	\
	for (i = 0; i < vector_size(mods); i++) \
	{ \
		mod = (module *) vector_elt(mods, i); \
		const modman_entry_##type##_t * modman_entry = modman_lookup_##type(mod); \
		assert(modman_entry); \
		if (!modman_entry->usage) \
		{ \
			r = DESTROY(mod); \
			assert(!r); \
			ndestroyed++; \
		} \
	} \
	vector_destroy(mods); \
	return ndestroyed; \
}

DESTROY_ALL(CFS_t, cfs);
DESTROY_ALL(LFS_t, lfs);
DESTROY_ALL(BD_t, bd);


#define DESTROYED_ALL_P(module, type) \
static bool destroyed_all_##type##_p(void) \
{ \
	modman_it_t it; \
	int r; \
	r = modman_it_init_##type(&it); \
	assert(!r); \
	return modman_it_next_##type(&it) == NULL; \
}

DESTROYED_ALL_P(CFS_t, cfs);
DESTROYED_ALL_P(LFS_t, lfs);
DESTROYED_ALL_P(BD_t, bd);


void destroy_all(void)
{
	size_t ndestroyed;

#if !DESTROY_ALL_ENABLED
	return;
#endif

	// TODO: detect [journal] cycles and destroy
	do {
		ndestroyed = destroy_all_cfs();
		ndestroyed += destroy_all_lfs();
		ndestroyed += destroy_all_bd();
	} while (ndestroyed);

	if (!destroyed_all_cfs_p())
		kdprintf(STDERR_FILENO, "%s: some CFS modules remain\n", __FUNCTION__);
	if (!destroyed_all_lfs_p())
		kdprintf(STDERR_FILENO, "%s: some LFS modules remain\n", __FUNCTION__);
	if (!destroyed_all_bd_p())
		kdprintf(STDERR_FILENO, "%s: some BD modules remain\n", __FUNCTION__);
}
