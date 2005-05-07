#include <inc/kfs_uses.h>
#include <kfs/table_classifier_cfs.h>
#include <kfs/cfs.h>
#include <kfs/lfs.h>
#include <kfs/bd.h>

#include <inc/hash_map.h>
#include <inc/hash_set.h>
#include <arch/simple.h>
#include <inc/assert.h>
#include <inc/stdio.h>

bool verbose = 0;


// Attempt to destroy every object in the map of nodes.
// Return the number of nodes destroyed.
int destroy_nodes(hash_set_t * nodes)
{
	hash_set_it_t it;
	kfs_node_t * node;
	int r;
	int ndestroyed = 0;

	hash_set_it_init(&it);

	while ((node = hash_set_next(nodes, &it)))
	{
		if (OBJFLAGS((object_t *) node->obj) & OBJ_PERSISTENT)
			continue;

		switch (node->type)
		{
			case NCFS:
			{
				CFS_t * cfs = node->obj;
				r = DESTROY(cfs);
				break;
			}
			case NLFS:
			{
				LFS_t * lfs = node->obj;
				r = DESTROY(lfs);
				break;
			}
			case NBD:
			{
				BD_t * bd = node->obj;
				r = DESTROY(bd);
				break;
			}
			default:
				panic("Unexpected type %d for use node %s\n", node->type, node->name);
		}

		if (r >= 0)
		{
			ndestroyed++;
			if (verbose)
				printf("destroyed %s\n", node->name);
		}
	}

	return ndestroyed;
}

// Create a copy of orig_graph that contains only nodes reachable from root.
void create_nodes_used_graph(hash_map_t * orig_graph, hash_set_t * new_graph, kfs_node_t * root)
{
	int r;
	size_t i;

	r = hash_set_insert(new_graph, root);
	assert(r >= 0);

	for (i=0; i < vector_size(root->uses); i++)
	{
		kfs_use_t * use = vector_elt(root->uses, i);
		create_nodes_used_graph(orig_graph, new_graph, use->node);
	}
}

// Copy nodes in nodes_used that are still alive into updated_nodes_used.
void update_nodes_used_graph(hash_set_t * nodes_used, hash_set_t * updated_nodes_used)
{
	hash_map_t * nodes;
	hash_set_it_t it;
	kfs_node_t * node_used;
	int r;

	nodes = kfs_uses();
	assert(nodes);
	hash_set_it_init(&it);

	while ((node_used = hash_set_next(nodes_used, &it)))
	{
		if (hash_map_find_val(nodes, node_used->obj))
		{
			r = hash_set_insert(updated_nodes_used, node_used);
			assert(r >= 0);
		}
	}
}


static void print_usage(const char * bin_name)
{
	printf("Usage: %s <mount> [-v]\n", bin_name);
}

void umain(int argc, const char ** argv)
{
	const char * mount;
	hash_map_t * uses_graph;
	CFS_t * tclass;
	kfs_node_t * tclass_node;
	kfs_node_t * node = NULL;
	kfs_use_t * use;
	int ndestroyed, i;

	// Parse arguments
	if (get_arg_idx(argc, argv, "-h") || argc < 2 || argc > 3)
	{
		print_usage(argv[0]);
		exit();
	}

	mount = argv[1];
	if (get_arg_idx(argc, argv, "-v"))
		verbose = 1;


	// Find the path to DESTROY
	uses_graph = kfs_uses();
	if (!uses_graph)
	{
		fprintf(STDERR_FILENO, "kfs_uses() failed\n");
		exit();
	}

	tclass = get_table_classifier();
	if (!tclass)
	{
		fprintf(STDERR_FILENO, "Unable to find root table classifier\n");
		exit();
	}

	tclass_node = hash_map_find_val(uses_graph, tclass);
	assert(tclass_node);
	for (i=0; i < vector_size(tclass_node->uses); i++)
	{
		use = vector_elt(tclass_node->uses, i);
		if (!strcmp(mount, use->name))
		{
			node = use->node;
			break;
		}
	}
	if (!node)
	{
		fprintf(STDERR_FILENO, "Unable to find mount at \"%s\"\n", mount);
		exit();
	}

	if (!table_classifier_cfs_remove(tclass, mount))
	{
		fprintf(STDERR_FILENO, "table_classifier_cfs_remove() failed to unmount %s pointing to %s\n", mount, node->name);
		exit();
	}
	if (verbose)
		printf("unmounted from table_classifier_cfs\n");

	
	// DESTROY the objects used by this mount point

	hash_set_t * nodes_used = hash_set_create();
	assert(nodes_used);
	create_nodes_used_graph(uses_graph, nodes_used, node);

	while ((ndestroyed = destroy_nodes(nodes_used)) > 0)
	{
		hash_set_t * updated_nodes_used = hash_set_create();
		assert(updated_nodes_used);

		update_nodes_used_graph(nodes_used, updated_nodes_used);

		hash_set_destroy(nodes_used);
		nodes_used = updated_nodes_used;
	}
}
