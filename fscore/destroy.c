#include <lib/platform.h>
#include <lib/vector.h>

#include <fscore/cfs.h>
#include <fscore/lfs.h>
#include <fscore/bd.h>
#include <fscore/journal_bd.h>
#include <fscore/modman.h>
#include <fscore/destroy.h>


// Destroy all modules of type 'module' that have no users.
// Note:
// Right now destroying modules does not cause data writes, but if modules
// begin to do this we will need to call patch_reclaim_written() before
// destroying blockman-using BDs.
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
			if (r) \
				fprintf(stderr, "%s(): failed to destroy %s\n", __FUNCTION__, modman_name_##type(mod)); \
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


// Destroy all journal_bd journal uses to ensure there are no journal-induced
// cycles. It is safe to just remove the journal use because fstitchd has synced.
void destroy_journal_uses(void)
{
	modman_it_t it;
	BD_t * bd;
	int r;

	r = modman_it_init_bd(&it);
	assert(!r);
	while ((bd = modman_it_next_bd(&it)))
	{
		if (!strncmp(modman_name_bd(bd), "journal_bd-", strlen("journal_bd-")))
		{
			r = journal_bd_set_journal(bd, NULL);
			assert(r >= 0);
		}
	}
}


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

	destroy_journal_uses();

	do {
		ndestroyed = destroy_all_cfs();
		ndestroyed += destroy_all_lfs();
		ndestroyed += destroy_all_bd();
	} while (ndestroyed);

	if (!destroyed_all_cfs_p())
		fprintf(stderr, "%s: some CFS modules remain\n", __FUNCTION__);
	if (!destroyed_all_lfs_p())
		fprintf(stderr, "%s: some LFS modules remain\n", __FUNCTION__);
	if (!destroyed_all_bd_p())
		fprintf(stderr, "%s: some BD modules remain\n", __FUNCTION__);
}
