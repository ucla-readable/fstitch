#include <inc/vector.h>
#include <inc/hash_map.h>
#include <inc/stdio.h>
#include <inc/malloc.h>
#include <arch/simple.h> // get_arg
#include <kfs/modman.h>

struct node;
typedef struct node node_t;

typedef struct {
	node_t * node;
	const char * name;
} use_t;

struct node {
	enum {NCFS, NLFS, NBD} type;
	void * obj;
	const char * name;
	vector_t * uses; // vector of use_t
};

node_t * node_create(int type, void * obj, const char * name)
{
	node_t * n = malloc(sizeof(*n));
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

use_t * use_create(node_t * node, const char * name)
{
	use_t * u = malloc(sizeof(*u));
	if (!u)
		return NULL;

	u->node = node;
	u->name = name;

	return u;
}

const char * typename(int type)
{
	switch (type)
	{
		case NCFS: return "CFS";
		case NLFS: return "LFS";
		case NBD:  return "BD ";
		default:   return "   ";
	}
}

const char * color(int type)
{
	switch (type)
	{
		case NCFS: return "springgreen";
		case NLFS: return "cyan3";
		case NBD:  return "slateblue1";
		default:   return "white";
	}
}

void output_graph_text(hash_map_t * nodes)
{
	hash_map_it_t * it;
	node_t * n;

	it = hash_map_it_create();
	assert(it);

	while ((n = hash_map_val_next(nodes, it)))
	{
		// output this node
		printf("%s  %s\n", typename(n->type), n->name);

		// output nodes used
		if (n->uses)
		{
			int i;
			for (i=0; i < vector_size(n->uses); i++)
			{
				use_t * use = (use_t *) vector_elt(n->uses, i);
				if (use->name && use->name[0])
					printf("         %s: %s\n", use->name, use->node->name);
				else
					printf("         %s\n", use->node->name);
			}
		}
	}

	hash_map_it_destroy(it);
}

void output_graph_dot(hash_map_t * nodes)
{
	hash_map_it_t * it;
	node_t * n;

	it = hash_map_it_create();
	assert(it);

	printf("digraph kfs\n");
	printf("{\n");
	printf("node [shape=box,color=black];\n");

	while ((n = hash_map_val_next(nodes, it)))
	{
		// output this node
		printf("n%u [", n);
		printf("label=\"%s\"", n->name);
		printf(",fillcolor=%s,style=filled", color(n->type));
		printf("]\n");

		// output nodes used
		if (n->uses)
		{
			size_t i;
			for (i=0; i < vector_size(n->uses); i++)
			{
				use_t * use = (use_t *) vector_elt(n->uses, i);
				printf("\tn%u -> n%u [label=\"%s\"];\n", n, use->node, use->name);
			}
		}
	}

	hash_map_it_destroy(it);

	printf("}\n");
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
			node_t * n;								\
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
			node_t * usern = hash_map_find_val(nodes, usert);			\
			assert(usern);												\
			r = vector_push_back(usern->uses, u);						\
			assert(r >= 0);												\
		}																\
	} while (0)

hash_map_t * build_graph()
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
		node_t * n;
		it = hash_map_it_create();
		assert(it);

		while ((n = hash_map_val_next(nodes, it)))
		{
			int i;
			use_t * u;
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


void print_usage(const char * binname)
{
	fprintf(STDERR_FILENO, "Usage: %s: [-t|-d]\n", binname);
}

void umain(int argc, const char ** argv)
{
	hash_map_t * graph;
	int text_dot = 1;

	if (get_arg_idx(argc, argv, "-h"))
	{
		print_usage(argv[0]);
		exit();
	}

	if (get_arg_idx(argc, argv, "-t"))
		text_dot = 0;
	else if (get_arg_idx(argc, argv, "-d"))
		text_dot = 1;

	graph = build_graph();
	if (graph)
	{
		if (text_dot == 0)
			output_graph_text(graph);
		else
			output_graph_dot(graph);
	}
}
