#include <inc/types.h>
#include <kfs/oo.h>

/* FIXME add change descriptors */

struct bdesc;
struct fdesc;
struct feature;

struct LFS;
typedef struct LFS LFS_t;

struct LFS {
	DESTRUCTOR(LFS_t);
	DECLARE(LFS_t, struct bdesc *, allocate_block, uint32_t size, int purpose);
	DECLARE(LFS_t, struct bdesc *, lookup_block, uint32_t number, uint32_t offset, uint32_t size);
	DECLARE(LFS_t, struct fdesc *, lookup_name, const char * name);
	DECLARE(LFS_t, struct bdesc *, get_file_block, struct fdesc * file, uint32_t offset);
	DECLARE(LFS_t, int, append_file_block, struct fdesc * file, struct bdesc * block);
	DECLARE(LFS_t, struct fdesc *, allocate_name, char * name, int type, struct fdesc * link);
	DECLARE(LFS_t, int, rename, const char * oldname, const char * newname);
	DECLARE(LFS_t, struct bdesc *, truncate_file_block, struct fdesc * file);
	DECLARE(LFS_t, int, free_block, struct bdesc * block);
	DECLARE(LFS_t, int, apply_changes, struct chdesc * changes);
	DECLARE(LFS_t, int, remove_name, const char * name);
	DECLARE(LFS_t, int, write_block, struct bdesc * block, uint32_t offset, uint32_t size, void * data);
	DECLARE(LFS_t, const struct feature *, get_features);
	DECLARE(LFS_t, int, get_metadata, struct fdesc * file, uint32_t id, ...);
	DECLARE(LFS_t, int, set_metadata, struct fdesc * file, uint32_t id, ...);
	DECLARE(LFS_t, int, sync, struct fdesc * file);
	void * instance;
};
