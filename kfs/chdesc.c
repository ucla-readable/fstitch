#include <inc/stdio.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/chdesc.h>
#include <kfs/bdesc.h>

chdesc_t * chdesc_create_noop(bdesc_t * block)
{
	chdesc_t * chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return NULL;
	chdesc->block = block;
	chdesc->type = NOOP;
	chdesc->dependencies = NULL;
	chdesc->dependents = NULL;
	chdesc->weak_refs = NULL;
	chdesc->flags = 0;
	return chdesc;
}

/* FIXME */
int chdesc_create_bit(bdesc_t * block, uint16_t offset, uint32_t xor);
int chdesc_create_byte(bdesc_t * block, uint16_t offset, uint16_t length, void * data);
int chdesc_create_init(bdesc_t * block);
int chdesc_create_full(bdesc_t * block, void * data);

static int chdesc_has_dependency(chdesc_t * dependent, chdesc_t * dependency)
{
	chmetadesc_t * meta;
	dependent->flags |= CHDESC_MARKED;
	for(meta = dependent->dependencies; meta; meta = meta->next)
	{
		if(meta->desc == dependency)
			return 1;
		if(!(meta->desc->flags & CHDESC_MARKED))
			if(chdesc_has_dependency(meta->desc, dependency))
				return 1;
	}
	/* the chdesc graph is a DAG, so unmarking here would defeat the purpose */
	return 0;
}

static void chdesc_unmark_graph(chdesc_t * root)
{
	chmetadesc_t * meta;
	root->flags &= ~CHDESC_MARKED;
	for(meta = root->dependencies; meta; meta = meta->next)
		if(meta->desc->flags & CHDESC_MARKED)
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

/* FIXME */
int chdesc_apply(chdesc_t * chdesc);
int chdesc_rollback(chdesc_t * chdesc);

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

int chdesc_weak_retain(chdesc_t * chdesc, chdesc_t ** location)
{
	chrefdesc_t * ref = malloc(sizeof(*ref));
	if(!ref)
		return -1;
	
	ref->desc = location;
	ref->next = chdesc->weak_refs;
	chdesc->weak_refs = ref;
	
	if(*location)
		chdesc_weak_release(location);
	*location = chdesc;
	
	return 0;
}

void chdesc_weak_release(chdesc_t ** location)
{
	chrefdesc_t ** prev = &(*location)->weak_refs;
	chrefdesc_t * scan = (*location)->weak_refs;
	while(scan && scan->desc != location)
	{
		prev = &scan->next;
		scan = scan->next;
	}
	if(!scan)
	{
		fprintf(STDERR_FILENO, "%s: weak release of non-weak chdesc pointer!\n", __FUNCTION__);
		return;
	}
	*prev = scan->next;
	*location = NULL;
	free(scan);
}

static void chdesc_weak_collect(chdesc_t * chdesc)
{
	while(chdesc->weak_refs)
	{
		/* in theory, this is all that is necessary... */
		if(*chdesc->weak_refs->desc == chdesc)
			chdesc_weak_release(chdesc->weak_refs->desc);
		else
		{
			/* ...but check for this anyway */
			chrefdesc_t * next = chdesc->weak_refs;
			fprintf(STDERR_FILENO, "%s: dangling chdesc weak reference!\n", __FUNCTION__);
			chdesc->weak_refs = next->next;
			free(next);
		}
	}
}

int chdesc_destroy(chdesc_t ** chdesc)
{
	if((*chdesc)->dependencies || (*chdesc)->dependents)
		return -1;
	chdesc_weak_collect(*chdesc);
	
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
	*chdesc = NULL;
	
	return 0;
}
