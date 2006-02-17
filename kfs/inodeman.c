#include <stdlib.h>
#include <string.h>
#include <inc/error.h>
#include <lib/stdio.h>
#include <lib/hash_map.h>

#include <kfs/inodeman.h>

#define INODEMAN_DEBUG 0

#if INODEMAN_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


static vector_t * mount_table = NULL;

// Return the cfs associated with the mount name is on, set transformed_name
// to be the filename this cfs expects
static CFS_t * lookup_cfs_name(vector_t * mount_table, const char * name, char ** transformed_name)
{
	Dprintf("%s(0x%08x, \"%s\", 0x%08x)\n", __FUNCTION__, mount_table, name, transformed_name);
	const size_t mount_table_size = vector_size(mount_table);
	const size_t name_len = strlen(name);
	size_t longest_match = 0;
	CFS_t * best_match = NULL;
	int i;

	for (i = 0; i < mount_table_size; i++)
	{
		const mount_entry_t *me = (mount_entry_t *) vector_elt(mount_table, i);
		size_t mount_len = strlen(me->path);

		if(mount_len <= longest_match || name_len < mount_len)
			continue;

		/* / needs special consideration because it has a trailing / */
		if(mount_len == 1)
			mount_len = 0;

		if(!strncmp(me->path, name, mount_len))
		{
			if(name[mount_len] && name[mount_len] != '/')
				continue;
			longest_match = mount_len;
			best_match = me->cfs;
		}
	}

	*transformed_name = (char *) &name[longest_match];
	return best_match;
}


// Find slash and turn it to NUL
static inline char* nullify_slash(char* p)
{
	while (*p) {
		if (*p == '/') {
			*p = 0;
			return (p+1);
		}
		p++;
	}

	return NULL;
}

// Skip over slashes.
static inline char* skip_slash(char* p)
{
	while (*p == '/')
		p++;
	return p;
}

int path_to_inode(const char * path, CFS_t ** cfs, inode_t * ino)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, path);
	int r, namelen;
	char * transformed_name, * filename;
	char path2[MAXPATHLEN];
	inode_t parent;

	if (!path || !cfs || !ino)
		return -E_INVAL;

	strncpy(path2, path, strlen(path) + 1);
	*cfs = lookup_cfs_name(mount_table, path2, &transformed_name);
	if (!cfs)
		return -E_NOT_FOUND;

	if ((r = CALL(*cfs, get_root, ino)) < 0)
		return r;

	transformed_name = skip_slash(transformed_name);
	namelen = strlen(transformed_name);

	while (namelen > 0) {
		filename = transformed_name;
		transformed_name = nullify_slash(filename);
		parent = *ino;
		if ((r = CALL(*cfs, lookup, parent, filename, ino)) < 0)
			return r;

		if (transformed_name) {
			transformed_name = skip_slash(transformed_name);
			namelen = strlen(transformed_name);
		}
		else
			break;
	}

	return 0;
}

int path_to_parent_and_name(const char * path, CFS_t ** cfs, inode_t * parent, char ** filename)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, path);
	int r, i, namelen;
	char parentname[MAXPATHLEN];

	if (!path || !cfs || !parent || !filename)
		return -E_INVAL;

	namelen = strlen(path);
	for (i = namelen - 1; i >= 0; i--) {
		if (path[i] == '/') {
			strncpy(parentname, path, i + 1);
			parentname[i+1] = 0;
			*filename = (char *) path + i + 1;
			break;
		}
	}
	if (strlen(*filename) < 1)
		return -E_NOT_FOUND;

	if ((r = path_to_inode(parentname, cfs, parent)) < 0)
		return r;

	return 0;
}

vector_t * get_mount_table()
{
	return mount_table;
}

void inodeman_shutdown()
{
	vector_destroy(mount_table);
	mount_table = NULL;
}

int inodeman_init(void)
{
	if (mount_table)
		return -E_BUSY;
	if (!(mount_table = vector_create()))
		return -E_NO_MEM;
	return 0;
}

