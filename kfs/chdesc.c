#include <inc/stdio.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/chdesc.h>
#include <kfs/bdesc.h>

/* create a new NOOP chdesc */
chdesc_t * chdesc_alloc(bdesc_t * block)
{
	chdesc_t * chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return NULL;
	chdesc->block = block;
	chdesc->refs = 0;
	chdesc->type = NOOP;
	chdesc->dependencies = NULL;
	chdesc->dependents = NULL;
	chdesc->marked = 0;
	return chdesc;
}

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

/* add a dependency to a change descriptor without checking for cycles */
int chdesc_add_depend_fast(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * meta;
	
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

/* add a dependency to a change descriptor */
int chdesc_add_depend(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * meta;
	
	/* first make sure it's not already there */
	for(meta = dependent->dependencies; meta; meta = meta->next)
		if(meta->desc == dependency)
			return 0;
	
	/* avoid creating a dependency loop */
	if(dependent == dependency || chdesc_has_dependency(dependency, dependent))
	{
		printf("%s(): (%s:%d): Avoided recursive dependency!\n", __FUNCTION__, __FILE__, __LINE__);
		return -1;
	}
	/* chdesc_has_dependency() marks the DAG rooted at "dependency" so we must unmark it */
	chdesc_unmark_graph(dependency);
	
	return chdesc_add_depend_fast(dependent, dependency);
}

static void chdesc_meta_remove(chmetadesc_t ** list, chdesc_t * chdesc)
{
	chmetadesc_t * scan = *list;
	while(scan)
	{
		if(scan->desc == chdesc)
		{
			*list = scan->next;
			free(scan);
			scan = *list;
			/* could return here, but keep going just to be sure */
		}
		else
		{
			list = &scan->next;
			scan = scan->next;
		}
	}
}

int chdesc_remove_depend(chdesc_t * dependent, chdesc_t * dependency)
{
	chdesc_meta_remove(&dependent->dependencies, dependency);
	chdesc_meta_remove(&dependency->dependents, dependent);
	return 0;
}

/* satisfy a change descriptor, i.e. remove it from all others that depend on it */
/* WARNING: this function should not be called (except by the dependency
 * manager) once a chdesc has been added to the dependency manager */
int chdesc_satisfy(chdesc_t * chdesc)
{
	while(chdesc->dependents)
	{
		chmetadesc_t * meta = chdesc->dependents;
		chdesc->dependents = meta->next;
		chdesc_meta_remove(&meta->desc->dependencies, chdesc);
		free(meta);
	}
	return 0;
}

void chdesc_retain(chdesc_t * chdesc)
{
	chdesc->refs++;
}

void chdesc_release(chdesc_t ** chdesc)
{
	if(--(*chdesc)->refs <= 0)
	{
		switch((*chdesc)->type)
		{
			case BIT:
				break;
			case BYTE:
				if((*chdesc)->byte.olddata)
					free((*chdesc)->byte.olddata);
				if((*chdesc)->byte.newdata)
					free((*chdesc)->byte.newdata);
				break;
			case NOOP:
				break;
		}
		memset(*chdesc, 0, sizeof(**chdesc));
		free(*chdesc);
	}
	*chdesc = NULL;
}
