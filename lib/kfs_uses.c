#include <inc/vector.h>
#include <inc/hash_map.h>
#include <inc/assert.h>
#include <inc/malloc.h>
#include <kfs/modman.h>
#include <inc/kfs_uses.h>


kfs_node_t * node_create(int type, void * obj, const char * name)
{
	kfs_node_t * n = malloc(sizeof(*n));
	if (!n)
		return NULL;

	n->type = type;
	n->obj  = obj;
	n->name = name;
	n->uses = vector_create();
	if (!n->uses)
	{
		free(n);
		return NULL;
	}

	return n;
}

kfs_use_t * use_create(kfs_node_t * node, const char * name)
{
	kfs_use_t * u = malloc(sizeof(*u));
	if (!u)
		return NULL;

	u->node = node;
	u->name = name;

	return u;
}


#define ADD_NODES(typel, typeu, typen)				\
	do {											\
		modman_it_t * mit;							\
		typeu##_t * t;								\
		mit = modman_it_create_##typel();			\
		assert(mit);								\
		while ((t = modman_it_next_##typel(mit)))	\
		{											\
			const modman_entry_##typel##_t * me;	\
			kfs_node_t * n;							\
													\
			me = modman_lookup_##typel(t);			\
			assert(me);								\
			assert(me->name);						\
													\
			n = node_create(typen, t, me->name);	\
			assert(n);								\
			r = hash_map_insert(nodes, t, n);		\
			assert(r >= 0);							\
		}											\
		modman_it_destroy(mit);						\
	} while(0)

#define ADD_USERS(typel, typeu)											\
	do {																\
		const modman_entry_##typel##_t * me = modman_lookup_##typel(n->obj); \
		assert(me);														\
		for (i=0; i < vector_size(me->users); i++)						\
		{																\
			u = use_create(n, vector_elt((vector_t *) me->use_names, i)); \
			assert(u);													\
			typeu##_t * usert = vector_elt((vector_t *) me->users, i);	\
			assert(usert);												\
			kfs_node_t * usern = hash_map_find_val(nodes, usert);		\
			assert(usern);												\
			r = vector_push_back(usern->uses, u);						\
			assert(r >= 0);												\
		}																\
	} while (0)

hash_map_t * kfs_uses()
{
	hash_map_t * nodes;
	int r;

	nodes = hash_map_create();
	if (!nodes)
		return NULL;

	// Add nodes
	ADD_NODES(cfs, CFS, 0);
	ADD_NODES(lfs, LFS, 1);
	ADD_NODES(bd,  BD,  2);

	// Add use links
	{
		hash_map_it_t * it;
		kfs_node_t * n;
		it = hash_map_it_create();
		assert(it);

		while ((n = hash_map_val_next(nodes, it)))
		{
			int i;
			kfs_use_t * u;
			switch (n->type)
			{
				case 0: ADD_USERS(cfs, CFS); break;
				case 1: ADD_USERS(lfs, LFS); break;
				case 2: ADD_USERS(bd,  BD);  break;
				default: assert(0);
			}
		}

		hash_map_it_destroy(it);
	}

	return nodes;
}


CFS_t * get_table_classifier()
{
	modman_it_t * it;
	CFS_t * c;
	const char tcc_name[] = "table_classifier_cfs-"; 
	const int tcc_name_len = strlen(tcc_name);

	it = modman_it_create_cfs();
	if (!it)
	{
		fprintf(STDERR_FILENO, "modman_it_create_cfs() failed\n");
		return NULL;
	}

	while ((c = modman_it_next_cfs(it)))
	{
		const char * name = modman_name_cfs(c);
		if (name && !strncmp(name, tcc_name, tcc_name_len))
			return c;
	}

	return NULL;
}
