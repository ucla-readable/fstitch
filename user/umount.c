#include <inc/kfs_uses.h>
#include <kfs/modman.h>
#include <kfs/mount_selector_cfs.h>
#include <kfs/journal_bd.h>
#include <kfs/cfs.h>
#include <kfs/lfs.h>
#include <kfs/bd.h>

#include <lib/hash_map.h>
#include <lib/hash_set.h>
#include <arch/simple.h>
#include <inc/assert.h>
#include <inc/stdio.h>

static bool verbose = 0;

#define JOURNAL_BD_NAME "journal_bd-"

// Attempt to destroy every object in the map of nodes.
// Return the number of nodes destroyed.
static int destroy_nodes(hash_set_t * nodes)
{
	hash_set_it_t it;
	kfs_node_t * node;
	int r;
	int ndestroyed = 0;

	hash_set_it_init(&it, nodes);

	while ((node = hash_set_next(&it)))
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
		else if (r != -E_BUSY)
			kdprintf(STDERR_FILENO, "Unexpected DESTROY(%s) error: %i\n", node->name, r);
	}

	return ndestroyed;
}

// Create a copy of orig_graph that contains only nodes reachable from root.
static void create_nodes_used_graph(hash_map_t * orig_graph, hash_set_t * new_graph, kfs_node_t * root)
{
	int r;
	size_t i;

	r = hash_set_insert(new_graph, root);
	assert(r >= 0);

	for (i=0; i < vector_size(root->uses); i++)
	{
		kfs_use_t * use = vector_elt(root->uses, i);
		if (!hash_set_exists(new_graph, use->node)) // cycles are possible
			create_nodes_used_graph(orig_graph, new_graph, use->node);
	}
}

// Copy nodes in nodes_used that are still alive into updated_nodes_used.
static void update_nodes_used_graph(hash_set_t * nodes_used, hash_set_t * updated_nodes_used)
{
	hash_map_t * nodes;
	hash_set_it_t it;
	kfs_node_t * node_used;
	int r;

	nodes = kfs_uses();
	assert(nodes);
	hash_set_it_init(&it, nodes_used);

	while ((node_used = hash_set_next(&it)))
	{
		if (hash_map_find_val(nodes, node_used->obj))
		{
			r = hash_set_insert(updated_nodes_used, node_used);
			assert(r >= 0);
		}
	}
}

static void remove_cycles(kfs_node_t * root, hash_map_t * uses_graph)
{
	const int journal_bd_name_len = strlen(JOURNAL_BD_NAME);
	const int num_uses = vector_size(root->uses);
	int r;

	if (root->type == NBD && num_uses == 2 && !strncmp(JOURNAL_BD_NAME, root->name, journal_bd_name_len))
	{
		BD_t * journal = root->obj;
		size_t uses_index;
		kfs_use_t * journalbd_use;

		// Find the journalbd node
		uses_index = 0;
		journalbd_use = vector_elt(root->uses, uses_index);
		if (strcmp(journalbd_use->name, "journal"))
		{
			uses_index = 1;
			journalbd_use = vector_elt(root->uses, uses_index);
			if (strcmp(journalbd_use->name, "journal"))
			{
				kdprintf(STDERR_FILENO, "%s: %s does not use a \"journal\", I don't understand\n", __FUNCTION__, root->name);
				exit(0);
			}
		}

		// NOTE: we could avoid destroying here when there is not a cycle,
		// but this would take work to detect and doesn't seem like
		// it'd be of any benefit.

		// Unset and destroy the journalbd
		// (destroy now because no one will be using it after this)
		r = journal_bd_set_journal(journal, NULL);
		if (r < 0)
			kdprintf(STDERR_FILENO, "%s: journal_bd_set_journal(%s, NULL): %i", __FUNCTION__, root->name, r);
		r = DESTROY((BD_t*) journalbd_use->node->obj);
		if (r < 0)
			kdprintf(STDERR_FILENO, "%s: DESTROY(%s): %i\n", __FUNCTION__, journalbd_use->node->name, r);

		if (verbose)
			printf("destroyed %s (to break possible cycle)\n", journalbd_use->node->name);

		// Remove journalbd from our root and uses_graph
		vector_erase(root->uses, uses_index);
		hash_map_erase(uses_graph, journalbd_use->node->obj);

		// Zero out and free memory to more easily catch someone still holding a ref
		// journalbd_use->node
		memset(journalbd_use->node->uses, 0, sizeof(*journalbd_use->node->uses));
		vector_destroy(journalbd_use->node->uses);
		memset(journalbd_use->node, 0, sizeof(*journalbd_use->node));
		free(journalbd_use->node);
		// journalbd_use
		memset(journalbd_use, 0, sizeof(*journalbd_use));
		free(journalbd_use);
	}
	else if (num_uses >= 1)
	{
		// Support >1 use when it becomes necessary. For now, no need.
		assert(num_uses == 1);

		kfs_node_t * child = ((kfs_use_t*) vector_elt(root->uses, 0))->node;

		// heuristic to check that we are not descending another mount point:
		// type does not increase
		if (root->type == NCFS
			|| (root->type == NLFS && child->type != NCFS)
			|| (root->type == NBD && child->type == NBD))
			remove_cycles(child, uses_graph);
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
	CFS_t * mselect;
	kfs_node_t * mselect_node;
	kfs_node_t * node = NULL;
	kfs_use_t * use;
	int ndestroyed, i;

	// Parse arguments
	if (get_arg_idx(argc, argv, "-h") || argc < 2 || argc > 3)
	{
		print_usage(argv[0]);
		exit(0);
	}

	mount = argv[1];
	if (get_arg_idx(argc, argv, "-v"))
		verbose = 1;


	// Find the path to DESTROY
	uses_graph = kfs_uses();
	if (!uses_graph)
	{
		kdprintf(STDERR_FILENO, "kfs_uses() failed\n");
		exit(0);
	}

	mselect = get_mount_selector();
	if (!mselect)
	{
		kdprintf(STDERR_FILENO, "Unable to find root table classifier\n");
		exit(0);
	}

	mselect_node = hash_map_find_val(uses_graph, mselect);
	assert(mselect_node);
	for (i=0; i < vector_size(mselect_node->uses); i++)
	{
		use = vector_elt(mselect_node->uses, i);
		if (!strcmp(mount, use->name))
		{
			node = use->node;
			break;
		}
	}
	if (!node)
	{
		kdprintf(STDERR_FILENO, "Unable to find mount at \"%s\"\n", mount);
		exit(0);
	}

	if (!mount_selector_cfs_remove(mselect, mount))
	{
		kdprintf(STDERR_FILENO, "mount_selector_cfs_remove() failed to unmount %s pointing to %s\n", mount, node->name);
		exit(0);
	}
	if (verbose)
		printf("unmounted from mount_selector_cfs\n");

	remove_cycles(node, uses_graph);

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
