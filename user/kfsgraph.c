#include <inc/vector.h>
#include <inc/hash_map.h>
#include <inc/assert.h>
#include <inc/stdio.h>
#include <inc/kfs_uses.h>
#include <arch/simple.h> // get_arg
#include <kfs/modman.h>


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
	kfs_node_t * n;

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
				kfs_use_t * use = (kfs_use_t *) vector_elt(n->uses, i);
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
	kfs_node_t * n;

	it = hash_map_it_create();
	assert(it);

	printf("digraph kfs\n");
	printf("{\n");
	printf("nodesep=0.15;\nranksep=0.15;\n");
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
				kfs_use_t * use = (kfs_use_t *) vector_elt(n->uses, i);
				printf("\tn%u -> n%u [label=\"%s\"];\n", n, use->node, use->name);
			}
		}
	}

	hash_map_it_destroy(it);

	printf("}\n");
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

	graph = kfs_uses();
	if (graph)
	{
		if (text_dot == 0)
			output_graph_text(graph);
		else
			output_graph_dot(graph);
	}
}
