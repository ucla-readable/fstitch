#include <inc/stdio.h>
#include <inc/malloc.h>

#include <kfs/chdesc.h>

static int chdesc_has_dependency(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * meta;
	dependent->marked = 1;
	for(meta = dependent->dependencies; meta; meta = meta->next)
	{
		if(meta->desc == dependency)
			return 1;
		if(!meta->desc->marked)
			if(chdesc_has_dependency(meta->desc, dependency))
				return 1;
	}
	/* the chdesc graph is a DAG, so unmarking here would defeat the purpose */
	return 0;
}

static void chdesc_unmark_graph(chdesc_t * root)
{
	chmetadesc_t * meta;
	root->marked = 0;
	for(meta = root->dependencies; meta; meta = meta->next)
		if(meta->desc->marked)
			chdesc_unmark_graph(meta->desc);
}

/* add a dependency to a change descriptor */
int chdesc_add_depend(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * meta;
	
	/* first make sure it's not already there */
	for(meta = dependent->dependencies; meta; meta = meta->next)
		if(meta->desc == dependency)
			return 0;
	
	/* avoid creating a dependency loop */
	if(chdesc_has_dependency(dependency, dependent))
	{
		printf("%s(): (%s:%d): Avoided recursive dependency!\n", __FUNCTION__, __FILE__, __LINE__);
		return -1;
	}
	/* chdesc_has_dependency() marks the DAG rooted at "dependency" so we must unmark it */
	chdesc_unmark_graph(dependency);
	
	/* add the dependency to the dependent */
	meta = malloc(sizeof(*meta));
	if(!meta)
		return -1;
	meta->desc = dependency;
	meta->next = dependent->dependencies;
	dependent->dependencies = meta;
	
	/* add the dependent to the dependency */
	meta = malloc(sizeof(*meta));
	if(!meta)
	{
		meta = dependent->dependencies;
		dependent->dependencies = meta->next;
		free(meta);
		return -1;
	}
	meta->desc = dependent;
	meta->next = dependency->dependents;
	dependency->dependents = meta;
	
	return 0;
}

static void chdesc_remove_depend(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * prev = NULL;
	chmetadesc_t * scan = dependent->dependencies;
	while(scan)
	{
		if(scan->desc == dependency)
		{
			if(prev)
				prev->next = scan->next;
			else
				dependent->dependencies = scan->next;
			free(scan);
			scan = prev->next;
			/* could return here, but keep going just to be sure */
		}
		else
		{
			prev = scan;
			scan = scan->next;
		}
	}
}

/* satisfy a change descriptor, i.e. remove it from all others that depend on it */
int chdesc_satisfy(chdesc_t * chdesc)
{
	while(chdesc->dependents)
	{
		chmetadesc_t * meta = chdesc->dependents;
		chdesc->dependents = meta->next;
		chdesc_remove_depend(meta->desc, chdesc);
		free(meta);
	}
	return 0;
}
