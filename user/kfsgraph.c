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

void output_graph_text(hash_map_t * nodes, int level)
{
	hash_map_it_t it;
	kfs_node_t * n;
	char tmp_str[100]; // 100 seems like a reasonable number
	int r;

	hash_map_it_init(&it, nodes);

	while ((n = hash_map_val_next(&it)))
	{
		// output this node
		printf("%s  %s", typename(n->type), n->name);
		if (level == -1)
			printf("\n");
		else
		{
			r = OBJCALL((object_t *) n->obj, get_config, level, tmp_str, sizeof(tmp_str));
			assert(r >= 0);
			printf(" [%s] ", tmp_str);

			r = OBJCALL((object_t *) n->obj, get_status, level, tmp_str, sizeof(tmp_str));
			assert(r >= 0);
			printf("[%s]\n", tmp_str);
		}

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
}

void output_graph_dot(hash_map_t * nodes, int level)
{
	hash_map_it_t it;
	kfs_node_t * n;
	char tmp_strc[100]; // 100 seems like a reasonable number
	char tmp_strs[100]; // 100 seems like a reasonable number
	int r;

	hash_map_it_init(&it, nodes);

	printf("digraph kfs\n");
	printf("{\n");
	printf("nodesep=0.15;\nranksep=0.15;\n");
	printf("node [shape=record,color=black];\n");

	while ((n = hash_map_val_next(&it)))
	{
		// output this node
		printf("n%u [", n);
		if (level == -1)
			printf("label=\"%s\"", n->name);
		else
		{
			r = OBJCALL((object_t *) n->obj, get_config, level, tmp_strc, sizeof(tmp_strc));
			assert(r >= 0);

			r = OBJCALL((object_t *) n->obj, get_status, level, tmp_strs, sizeof(tmp_strs));
			assert(r >= 0);

			if (tmp_strc[0] ||tmp_strs[0])
				printf("label=\"{ %s |{%s|%s}}\"", n->name, tmp_strc, tmp_strs);
			else
				printf("label=\"%s\"", n->name);
		}
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

	printf("}\n");
}


void print_usage(const char * binname)
{
	fprintf(STDERR_FILENO, "Usage: %s: [-t|-d] [-l <level>]\n", binname);
}

void umain(int argc, const char ** argv)
{
	hash_map_t * graph;
	int text_dot = 1;
	int level;
	const char * level_str;

	if (get_arg_idx(argc, argv, "-h"))
	{
		print_usage(argv[0]);
		exit();
	}

	if (get_arg_idx(argc, argv, "-t"))
		text_dot = 0;
	else if (get_arg_idx(argc, argv, "-d"))
		text_dot = 1;

	if (text_dot == 0)
		level = -1;
	else
		level = CONFIG_BRIEF;

	if ((level_str = get_arg_val(argc, argv, "-l")))
		level = strtol(level_str, NULL, 10);

	graph = kfs_uses();
	if (graph)
	{
		if (text_dot == 0)
			output_graph_text(graph, level);
		else
			output_graph_dot(graph, level);
	}
}
