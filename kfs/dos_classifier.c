#include <inc/lib.h>
#include <inc/malloc.h>

#include <kfs/cfs.h>
#include <kfs/uhfs.h>

#define CLASS_DEBUG 1

#if CLASS_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

struct class_state {
	CFS_t * cfs1;
	const char * p1;
	CFS_t * cfs2;
	const char * p2;
};

static int xlate[UHFS_MAX_OPEN*2];

static int put(int fd, int side) {
	int i;
	for (i = 0; i < UHFS_MAX_OPEN; i++) {
		if (xlate[i*2] == -1) {
			xlate[i*2] = fd;
			xlate[i*2 + 1] = side;
			return 0;
		}
	}
	return -1;
}

static int get(int fd) {
	int i;
	for (i = 0; i < UHFS_MAX_OPEN; i++) {
		if (xlate[i*2] == fd)
			return xlate[i*2+1];
	}
	return -1;
}

static int del(int fd) {
	int i;
	for (i = 0; i < UHFS_MAX_OPEN; i++) {
		if (xlate[i*2] == fd) {
			xlate[i*2] = xlate[i*2+1] = -1;
			return 0;
		}
	}
	return -1;
}

static int class_open(CFS_t * cfs, const char * name, int mode, void * page)
{
	int r;
	int fd;
	struct class_state * state = (struct class_state *) cfs->instance;
	if (name[0] == '/')
		name++;
	if (!strncmp(name, "A:", 2)) {
		fd = CALL(state->cfs1, open, name+2, mode, page);
		if (fd < 0) return fd;
		r = put(fd, 0);
		if (r < 0) {
			Dprintf("%s()\n", __FUNCTION__);
			Dprintf("Can't save file descriptor!\n");
			return -E_UNSPECIFIED;
		}
		return fd;
	} else if (!strncmp(name, "C:", 2)) {
		fd = CALL(state->cfs2, open, name+2, mode, page);
		if (fd < 0) return fd;
		r = put(fd, 1);
		if (r < 0) {
			Dprintf("%s()\n", __FUNCTION__);
			Dprintf("Can't save file descriptor!\n");
			return -E_UNSPECIFIED;
		}
		return fd;
	}
	return -E_NO_DEV;
}

static int class_close(CFS_t * cfs, int fid)
{
	int r;
	struct class_state * state = (struct class_state *) cfs->instance;
	int side = get(fid);
	if (side == 0) {
		r = CALL(state->cfs1, close, fid);
		if(!r)
			del(fid);
		return r;
	} else if (side == 1) {
		r = CALL(state->cfs2, close, fid);
		if(!r)
			del(fid);
		return r;
	}
	return -E_NOT_FOUND;
}

static int class_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	struct class_state * state = (struct class_state *) cfs->instance;
	int side = get(fid);
	if (side == 0) {
		return CALL(state->cfs1, read, fid, data, offset, size);
	} else if (side == 1) {
		return CALL(state->cfs2, read, fid, data, offset, size);
	}
	return -E_NOT_FOUND;
}

static int class_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	struct class_state * state = (struct class_state *) cfs->instance;
	int side = get(fid);
	if (side == 0) {
		return CALL(state->cfs1, write, fid, data, offset, size);
	} else if (side == 1) {
		return CALL(state->cfs2, write, fid, data, offset, size);
	}
	return -E_NOT_FOUND;
}

static int class_getdirentries(CFS_t * cfs, int fid, char * buf, int nbytes, uint32_t * basep)
{
	struct class_state * state = (struct class_state *) cfs->instance;
	int side = get(fid);
	if (side == 0) {
		return CALL(state->cfs1, getdirentries, fid, buf, nbytes, basep);
	} else if (side == 1) {
		return CALL(state->cfs2, getdirentries, fid, buf, nbytes, basep);
	}
	return -E_NOT_FOUND;
}

static int class_truncate(CFS_t * cfs, int fid, uint32_t size)
{
	struct class_state * state = (struct class_state *) cfs->instance;
	int side = get(fid);
	if (side == 0) {
		return CALL(state->cfs1, truncate, fid, size);
	} else if (side == 1) {
		return CALL(state->cfs2, truncate, fid, size);
	}
	return -E_NOT_FOUND;
}

static int class_unlink(CFS_t * cfs, const char * name)
{
	int r;
	struct class_state * state = (struct class_state *) cfs->instance;
	if (name[0] == '/')
		name++;
	if (!strncmp(name, "A:", 2)) {
		r = CALL(state->cfs1, unlink, name+2);
		return r;
	} else if (!strncmp(name, "C:", 2)) {
		r = CALL(state->cfs2, unlink, name+2);
		return r;
	}
	return -E_NO_DEV;
}

static int class_link(CFS_t * cfs, const char * oldname, const char * newname)
{
	int r;
	struct class_state * state = (struct class_state *) cfs->instance;
	panic("unimplemented");
	if (!strncmp(oldname, "A:", 2)) {
		r = CALL(state->cfs1, link, oldname+2, newname+2);
		return r;
	} else if (!strncmp(oldname, "C:", 2)) {
		r = CALL(state->cfs2, link, oldname+2, newname+2);
		return r;
	}
	return -E_NO_DEV;
}

static int class_rename(CFS_t * cfs, const char * oldname, const char * newname)
{
	int r;
	struct class_state * state = (struct class_state *) cfs->instance;
	panic("unimplemented");
	if (!strncmp(oldname, "A:", 2)) {
		r = CALL(state->cfs1, rename, oldname+2, newname+2);
		return r;
	} else if (!strncmp(oldname, "C:", 2)) {
		r = CALL(state->cfs2, rename, oldname+2, newname+2);
		return r;
	}
	return -E_NO_DEV;
}

static int class_mkdir(CFS_t * cfs, const char * name)
{
	int r;
	struct class_state * state = (struct class_state *) cfs->instance;
	if (name[0] == '/')
		name++;
	if (!strncmp(name, "A:", 2)) {
		r = CALL(state->cfs1, mkdir, name+2);
		return r;
	} else if (!strncmp(name, "C:", 2)) {
		r = CALL(state->cfs2, mkdir, name+2);
		return r;
	}
	return -E_NO_DEV;
}

static int class_rmdir(CFS_t * cfs, const char * name)
{
	int r;
	struct class_state * state = (struct class_state *) cfs->instance;
	if (name[0] == '/')
		name++;
	if (!strncmp(name, "A:", 2)) {
		r = CALL(state->cfs1, rmdir, name+2);
		return r;
	} else if (!strncmp(name, "C:", 2)) {
		r = CALL(state->cfs2, rmdir, name+2);
		return r;
	}
	return -E_NO_DEV;
}

static size_t class_get_num_features(CFS_t * cfs, const char * name)
{
	size_t r;
	struct class_state * state = (struct class_state *) cfs->instance;
	if (name[0] == '/')
		name++;
	if (!strncmp(name, "A:", 2)) {
		r = CALL(state->cfs1, get_num_features, name+2);
		return r;
	} else if (!strncmp(name, "C:", 2)) {
		r = CALL(state->cfs2, get_num_features, name+2);
		return r;
	}
	return -E_NO_DEV;
}

static const feature_t * class_get_feature(CFS_t * cfs, const char * name, size_t num)
{
	const feature_t *r;
	struct class_state * state = (struct class_state *) cfs->instance;
	if (name[0] == '/')
		name++;
	if (!strncmp(name, "A:", 2)) {
		r = CALL(state->cfs1, get_feature, name+2, num);
		return r;
	} else if (!strncmp(name, "C:", 2)) {
		r = CALL(state->cfs2, get_feature, name+2, num);
		return r;
	}
	return (feature_t *)(-E_NO_DEV);
}

static int class_get_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	int r;
	struct class_state * state = (struct class_state *) cfs->instance;
	if (name[0] == '/')
		name++;
	if (!strncmp(name, "A:", 2)) {
		r = CALL(state->cfs1, get_metadata, name+2, id, size, data);
		return r;
	} else if (!strncmp(name, "C:", 2)) {
		r = CALL(state->cfs2, get_metadata, name+2, id, size, data);
		return r;
	}
	return -E_NO_DEV;
}

static int class_set_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t size, const void * data)
{
	int r;
	struct class_state * state = (struct class_state *) cfs->instance;
	if (name[0] == '/')
		name++;
	if (!strncmp(name, "A:", 2)) {
		r = CALL(state->cfs1, set_metadata, name+2, id, size, data);
		return r;
	} else if (!strncmp(name, "C:", 2)) {
		r = CALL(state->cfs2, set_metadata, name+2, id, size, data);
		return r;
	}
	return -E_NO_DEV;
}

static int class_sync(CFS_t * cfs, const char * name)
{
	int r;
	struct class_state * state = (struct class_state *) cfs->instance;
	if (name[0] == '/')
		name++;
	if (!strncmp(name, "A:", 2)) {
		r = CALL(state->cfs1, sync, name+2);
		return r;
	} else if (!strncmp(name, "C:", 2)) {
		r = CALL(state->cfs2, sync, name+2);
		return r;
	}
	return -E_NO_DEV;
}

static int class_destroy(CFS_t * cfs)
{
	free(cfs->instance);
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}

CFS_t * dos_classifier(CFS_t * cfs1, const char * p1, CFS_t * cfs2, const char * p2)
{
	struct class_state * state;
	CFS_t * cfs;
	
	cfs = malloc(sizeof(*cfs));
	if(!cfs)
		return NULL;
	
	state = malloc(sizeof(*state));
	if(!state)
		goto error_class;
	cfs->instance = state;
	
	ASSIGN(cfs, class, open);
	ASSIGN(cfs, class, close);
	ASSIGN(cfs, class, read);
	ASSIGN(cfs, class, write);
	ASSIGN(cfs, class, getdirentries);
	ASSIGN(cfs, class, truncate);
	ASSIGN(cfs, class, unlink);
	ASSIGN(cfs, class, link);
	ASSIGN(cfs, class, rename);
	ASSIGN(cfs, class, mkdir);
	ASSIGN(cfs, class, rmdir);
	ASSIGN(cfs, class, get_num_features);
	ASSIGN(cfs, class, get_feature);
	ASSIGN(cfs, class, get_metadata);
	ASSIGN(cfs, class, set_metadata);
	ASSIGN(cfs, class, sync);
	ASSIGN_DESTROY(cfs, class, destroy);
	
	state->cfs1 = cfs1;
	state->p1 = p1;
	state->cfs2 = cfs2;
	state->p2 = p2;
	
	int i;
	for (i = 0; i < UHFS_MAX_OPEN; i++) {
		xlate[i*2] = -1;
		xlate[i*2 + 1] = -1;
	}

	return cfs;
	
 error_class:
	free(cfs);
	return NULL;
}
