#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <lib/hash_map.h>
#include <lib/kdprintf.h>
#include <lib/panic.h>
#include <lib/vector.h>
#include <kfs/fuse_serve_inode.h>

#define FUSE_SERVE_INODE_DEBUG 0

#if FUSE_SERVE_INODE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


// Return the full name for local_name in directory parent.
// It is the caller's job to free the returned string.
// Returns NULL on out of memory or non-existant parent.
char * fname(fuse_ino_t parent, const char * local_name)
{
	char * full_name;

	if (parent == FUSE_ROOT_ID && !strcmp(local_name, "/"))
	{
		full_name = strdup(local_name);
		if (!full_name)
			return NULL;
	}
	else
	{
		const char * parent_full_name;
		size_t parent_full_name_len;
		size_t delim_len;
		size_t local_name_len;

		parent_full_name = inode_fname(parent);
		if (!parent_full_name)
		{
			Dprintf("%s(parent = %d, local_name = \"%s\") -> PARENT DOES NOT EXIST\n", __FUNCTION__, parent, local_name);
			return NULL;
		}

		parent_full_name_len = strlen(parent_full_name);
		delim_len = (parent_full_name_len == 1) ? 0 : 1;
		local_name_len = strlen(local_name);

		full_name = malloc(parent_full_name_len + delim_len + local_name_len + 1);
		if (!full_name)
			return NULL;

		memcpy(full_name, parent_full_name, parent_full_name_len);
		if (delim_len)
			full_name[parent_full_name_len] = '/';
		memcpy(full_name + parent_full_name_len + delim_len, local_name, local_name_len);
		full_name[parent_full_name_len + delim_len + local_name_len] = 0;
	}

	//Dprintf("%s(parent = %d, local_name = \"%s\") -> \"%s\"\n", __FUNCTION__, parent, local_name, full_name);
	return full_name;
}


typedef struct inoentry {
	const char * local_name;
	fuse_ino_t ino;
} inoentry_t;

struct inodes {
	uint32_t ino_counter; // value for next inode

	// fuse_ino_t ino -> char * filename
	hash_map_t * fnames;

	// fuse_ino_t child -> fuse_ino_t parent
	hash_map_t * parents;

	// fuse_ino_t parent -> vector_t * (inoentry)
	hash_map_t * lnames;
};

static inodes_t * curi = NULL;


void fuse_serve_inodes_set_cur(inodes_t * i)
{
	curi = i;
}

void fuse_serve_inodes_clear_cur(void)
{
	curi = NULL;
}


// Returns ino's full name. Returns NULL if ino does not exist.
const char * inode_fname(fuse_ino_t ino)
{
	return (const char *) hash_map_find_val(curi->fnames, (void *) ino);
}

// Returns ino's parent inode. Returns FAIL_INO if ino does not exist.
fuse_ino_t inode_parent(fuse_ino_t ino)
{
	return (fuse_ino_t) hash_map_find_val(curi->parents, (void *) ino);
}

// Returns the inode for local_name (which is in parent).
// Returns FAIL_INO if local_name does not exist in parent.
fuse_ino_t lname_inode(fuse_ino_t parent, const char * local_name)
{
	const vector_t * inodes;
	size_t size;
	size_t i;

	inodes = (const vector_t *) hash_map_find_val(curi->lnames, (void *) parent);
	if (!inodes)
		return FAIL_INO;

	size = vector_size(inodes);
	for (i=0; i < size; i++)
	{
		inoentry_t * entry = vector_elt(inodes, i);
		if (!strcmp(local_name, entry->local_name))
			return entry->ino;
	}
	return FAIL_INO;
}

// Create an inode for name and set its parent.
// Returns:
//   Success: 0 if new or -1 if exists; if ino != NULL then sets *ino
//   Failure: -ENOMEM
int add_inode(fuse_ino_t parent, const char * local_name, fuse_ino_t * pino)
{
	char * full_name;
	fuse_ino_t ino;
	inoentry_t * entry;
	vector_t * linodes;
	int r;

	assert(local_name && (parent != FAIL_INO) && pino);

	if (lname_inode(parent, local_name) != FAIL_INO)
	{
		Dprintf("%s(parent = %lu, local_name = \"%s\") -> PARENT DOES NOT EXIST\n", __FUNCTION__, parent, local_name);
		return -1;
	}

	full_name = fname(parent, local_name);
	if (!full_name)
		return -ENOMEM;

	// store in our inode structures

	ino = (fuse_ino_t) curi->ino_counter++;

	entry = malloc(sizeof(*entry));
	assert(entry);

	entry->local_name = strdup(local_name);
	entry->ino = ino;

	linodes = (vector_t *) hash_map_find_val(curi->lnames, (void *) parent);
	if (!linodes)
	{
		linodes = vector_create();
		assert(linodes);
		r = hash_map_insert((void *) curi->lnames, (void *) parent, (void *) linodes);
		assert(r == 0);
	}
	r = vector_push_back(linodes, entry);
	assert(r >= 0);

	r = hash_map_insert(curi->fnames, (void *) ino, (void *) full_name);
	assert(r == 0);

	r = hash_map_insert(curi->parents, (void *) ino, (void *) parent);
	assert(r == 0);

	*pino = ino;
	Dprintf("%s(parent = %lu, local_name = \"%s\") -> inode %lu\n", __FUNCTION__, parent, local_name, ino);
	return 0;
}

void remove_inode(fuse_ino_t ino)
{
	char * full_name;
	fuse_ino_t parent;
	vector_t * linodes;
	size_t size;
	size_t i;

	full_name = hash_map_erase(curi->fnames, (void *) ino);
	if (!full_name)
	{
		kdprintf(STDERR_FILENO, "%s(ino = %lu): ino does not exist\n", __FUNCTION__, ino);
		return;
	}
	memset(full_name, 0, strlen(full_name));
	free(full_name);

	parent = (fuse_ino_t) hash_map_erase(curi->parents, (void *) ino);
	assert(parent != FAIL_INO);

	linodes = hash_map_find_val(curi->lnames, (void *) parent);
	size = vector_size(linodes);
	for (i=0; i < size; i++)
	{
		inoentry_t * entry = vector_elt(linodes, i);
		if (entry->ino == ino)
		{
			memset((char*) entry->local_name, 0, strlen(entry->local_name));
			free((char *) entry->local_name);
			free(entry);
			vector_erase(linodes, i);

			if (size == 1)
			{
				if(!hash_map_erase(curi->lnames, (void *) parent))
					assert(0);
				vector_destroy(linodes);
			}

			Dprintf("%s(ino = %lu)\n", __FUNCTION__, ino);
			return;
		}
	}

	assert(0);
}

static void inodes_shutdown(void)
{
	assert(curi);
	if (curi->parents && curi->fnames && curi->lnames)
	{
		// FUSE does not guarantee that all looked up inodes will be released.
		// We can free these inodes here so that we don't trip memory leak
		// detectors; the value of this is questionable.

		vector_t * inos;
		hash_map_it_t it;
		vector_t * inoentries;
		size_t i;

		inos = vector_create();
		assert(inos);

		hash_map_it_init(&it, curi->lnames);

		while ((inoentries = (vector_t *) hash_map_val_next(&it)))
		{
			for (i=0; i < vector_size(inoentries); i++)
			{
				inoentry_t * e = (inoentry_t *) vector_elt(inoentries, i);
				vector_push_back(inos, (void *) e->ino);
			}
		}

		for (i=0; i < vector_size(inos); i++)
			remove_inode((fuse_ino_t) vector_elt(inos, i));
		vector_destroy(inos);
	}

	if (curi->parents)
	{
		hash_map_destroy(curi->parents);
		curi->parents = NULL;
	}
	if (curi->fnames)
	{
		hash_map_destroy(curi->fnames);
		curi->fnames = NULL;
	}
	if (curi->lnames)
	{
		hash_map_destroy(curi->lnames);
		curi->lnames = NULL;
	}
}


void fuse_serve_inodes_destroy(inodes_t * i)
{
	if (!i)
		return;
	curi = i;
	inodes_shutdown();
	memset(curi, 0, sizeof(*curi));
	free(curi);
	curi = NULL;
}

inodes_t * fuse_serve_inodes_create(void)
{
	fuse_ino_t root_ino;
	int r;

	static_assert(sizeof(void*) == sizeof(fuse_ino_t));

//	assert(!fnames && !parents && !lnames);

	inodes_t * i;
	assert(!curi);

	if (!(curi = calloc(1, sizeof(*i))))
		return NULL;

	curi->ino_counter = 1;

	curi->lnames = hash_map_create();
	if (!curi->lnames)
	{
//		inodes_shutdown();
		return NULL;
	}

	curi->fnames = hash_map_create();
	if (!curi->fnames)
	{
//		inodes_shutdown();
		return NULL;
	}

	curi->parents = hash_map_create();
	if (!curi->parents)
	{
//		inodes_shutdown();
		return NULL;
	}

	if ((r = add_inode(FUSE_ROOT_ID, "/", &root_ino)) < 0)
	{
//		inodes_shutdown();
		return NULL;
	}
	assert(root_ino == FUSE_ROOT_ID);

	i = curi;
	curi = NULL;
	return i;
}
