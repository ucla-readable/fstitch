#include <inc/lib.h>
#include <inc/serial_cfs.h>
#include <inc/cfs_ipc_client.h>

const char kfsd_name_sh[] = "/kfsd";
const char kfsd_name_kern[] = "kfsd";

static envid_t
find_fs(void)
{
	size_t ntries;
	size_t i;

	// Try to find fs a few times, in case this env is being
	// started at the same time as fs, thus giving fs time to do its
	// fork.
	// 20 is most arbitrary: 10 worked in bochs, so I doubled to get 20.
	// NOTE: netclient.c:find_netd_ipcrecv() does the same.
	for (ntries = 0; ntries < 20; ntries++)
	{
		for (i = 0; i < NENV; i++)
		{
			//if (envs[i].env_status != ENV_FREE)
			//	printf("find_fs: name: [%s]\n", envs[i].env_name);
			if (envs[i].env_status != ENV_FREE &&
				(!strncmp(envs[i].env_name, kfsd_name_sh, strlen(kfsd_name_sh))
				 || !strncmp(envs[i].env_name, kfsd_name_kern, strlen(kfsd_name_kern))))
				return envs[i].env_id;
		}
		sys_yield();
	}

	return 0;
}

#define REQVA (0x10000000 - PGSIZE)

static char ipc_page[PGSIZE * 2];

int
cfs_open(const char *fname, int mode, void *refpg)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_open *pg = (struct Scfs_open*)
		ROUNDUP32(ipc_page, PGSIZE);
   	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_OPEN;
	pg->mode = mode;
	strncpy(pg->path, fname, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	ipc_send(fsid, 0, refpg, PTE_U|PTE_P, NULL);

	//if (get_pte((void*) REQVA) & PTE_P)
	//	panic("kpl ipcrecv: REQVA already mapped\n");

	do {
		r = ipc_recv(fsid, &from, 0, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	return r;
}

int
cfs_close(int fid)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_close *pg = (struct Scfs_close*)
		ROUNDUP32(ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_CLOSE;
	pg->fid = fid;

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	//if (get_pte((void*) REQVA) & PTE_P)
	//	panic("kpl ipcrecv: REQVA already mapped\n");

	do {
		r = ipc_recv(fsid, &from, 0, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	return r;
}

int
cfs_read(int fid, uint32_t offset, uint32_t size, char *data)
{
	int r = 0;
	envid_t fsid;
	envid_t from;
	uint32_t perm;
	uint32_t i;

	fsid = find_fs();

	for (i = 0; i < size; i += PGSIZE) {
		struct Scfs_read *pg = (struct Scfs_read*)
			ROUNDUP32(ipc_page, PGSIZE);
		memset(pg, 0, PGSIZE);
		pg->scfs_type = SCFS_READ;
		pg->fid = fid;
		pg->offset = offset + i;
		pg->size = MIN(size - i, PGSIZE);

		ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

		if (get_pte((void*) REQVA) & PTE_P)
			panic("kpl ipcrecv: REQVA already mapped\n");

		do {
			r = ipc_recv(fsid, &from, (void*)REQVA, &perm, 0);
			assert(from == fsid);
			if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
		} while (from != fsid);
		if (r < 0) return r;
		memcpy(data + i, (void*)REQVA, MIN(size-i, PGSIZE));
		sys_page_unmap(0, (void*)REQVA);
		if (r < MIN(size - i, PGSIZE))
			return i+r;
	}
	return size;
}

int
cfs_write(int fid, uint32_t offset, uint32_t size, const char *data)
{
	int r = 0;
	envid_t fsid;
	envid_t from;
	uint32_t perm;
	uint32_t i;

	fsid = find_fs();

	for (i = 0; i < size; i += PGSIZE) {
		struct Scfs_write *pg = (struct Scfs_write*)
			ROUNDUP32(ipc_page, PGSIZE);
		memset(pg, 0, PGSIZE);
		pg->scfs_type = SCFS_WRITE;
		pg->fid = fid;
		pg->offset = offset + i;
		pg->size = MIN(size - i, PGSIZE);

		ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);
		r = sys_page_unmap(0, pg);
		if (r < 0) panic("%s:%d\n", __FILE__, __LINE__);
		r = sys_page_alloc(0, pg, PTE_W|PTE_U|PTE_P);
		if (r < 0) panic("%s:%d\n", __FILE__, __LINE__);

		memcpy(pg, data + i, MIN(size - i, PGSIZE));

		ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

		do {
			r = ipc_recv(fsid, &from, 0, &perm, 0);
			assert(from == fsid);
			if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
		} while (from != fsid);
		if (r < MIN(size - i, PGSIZE))
			return i+r;
	}
	return size;
}

int
cfs_getdirentries(int fid, char * buf, size_t nbytes, off_t *basep)
{
	int r = 0, nbytes_read = 0;
	envid_t fsid;
	uint32_t perm;
	struct Scfs_getdirentries_return *ret = (struct Scfs_getdirentries_return*) REQVA;

	fsid = find_fs();

	struct Scfs_getdirentries *pg = (struct Scfs_getdirentries*)
		ROUNDUP32(ipc_page, PGSIZE);
	while (nbytes_read < nbytes)
	{
		memset(pg, 0, PGSIZE);
		pg->scfs_type = SCFS_GETDIRENTRIES;
		pg->fid = fid;
		pg->nbytes = nbytes - nbytes_read;
		pg->basep = *basep;

		ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);
		
		assert(!(get_pte((void*) ret) & PTE_P));
		r = ipc_recv(fsid, NULL, (void*) ret, &perm, 0);
		if (r < 0)
			goto exit;
		
		if (!ret->nbytes_read)
			goto exit;

		assert(nbytes_read + ret->nbytes_read <= nbytes);
		memcpy(buf+nbytes_read, &ret->buf, ret->nbytes_read);

		*basep = ret->basep;
		nbytes_read += ret->nbytes_read;
		sys_page_unmap(0, ret);
	}

  exit:
	sys_page_unmap(0, ret);
	return nbytes_read ? nbytes_read : r;
}

int
cfs_truncate(int fid, uint32_t size)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_truncate *pg = (struct Scfs_truncate*)
		ROUNDUP32(ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_TRUNCATE;
	pg->fid = fid;
	pg->size = size;

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	do {
		r = ipc_recv(fsid, &from, 0, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	return r;
}

int
cfs_unlink(const char *name)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_unlink *pg = (struct Scfs_unlink*)
		ROUNDUP32(ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_UNLINK;
	strncpy(pg->name, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	do {
		r = ipc_recv(fsid, &from, 0, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	return r;
}

int
cfs_link(const char *oldname, const char *newname)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_link *pg = (struct Scfs_link*)
		ROUNDUP32(ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_LINK;
	strncpy(pg->oldname, oldname, MIN(SCFSMAXNAMELEN, MAXNAMELEN));
	strncpy(pg->newname, newname, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	do {
		r = ipc_recv(fsid, &from, 0, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	return r;
}

int
cfs_rename(const char *oldname, const char *newname)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_rename *pg = (struct Scfs_rename*)
		ROUNDUP32(ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_RENAME;
	strncpy(pg->oldname, oldname, MIN(SCFSMAXNAMELEN, MAXNAMELEN));
	strncpy(pg->newname, newname, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	do {
		r = ipc_recv(fsid, &from, 0, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	return r;
}

int
cfs_mkdir(const char *name)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_mkdir *pg = (struct Scfs_mkdir*)
		ROUNDUP32(ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_MKDIR;
	strncpy(pg->path, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	do {
		r = ipc_recv(fsid, &from, 0, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	return r;
}

int
cfs_rmdir(const char *name)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_rmdir *pg = (struct Scfs_rmdir*)
		ROUNDUP32(ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_RMDIR;
	strncpy(pg->path, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	do {
		r = ipc_recv(fsid, &from, 0, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	return r;
}

int
cfs_get_num_features(char *name)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_get_num_features *pg = (struct Scfs_get_num_features*)
		ROUNDUP32(ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_GET_NUM_FEATURES;
	strncpy(pg->name, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	do {
		r = ipc_recv(fsid, &from, 0, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	return r;
}

int
cfs_get_feature(char *name, int num, char *dump)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_get_feature *pg = (struct Scfs_get_feature*)
		ROUNDUP32(ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_GET_FEATURE;
	strncpy(pg->name, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));
	pg->num = num;

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	if (get_pte((void*) REQVA) & PTE_P)
		panic("kpl ipcrecv: REQVA already mapped\n");
	
	do {
		r = ipc_recv(fsid, &from, (void*)REQVA, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	memcpy(dump, (void*)REQVA, PGSIZE);
	sys_page_unmap(0, (void*)REQVA);
	return r;
}

int
cfs_get_metadata(const char *name, int id, struct Scfs_metadata *md)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;
	struct Scfs_metadata *p;

	fsid = find_fs();

	struct Scfs_get_metadata *pg = (struct Scfs_get_metadata*)
		ROUNDUP32(ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_GET_METADATA;
	pg->id = id;
	strncpy(pg->name, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	if (get_pte((void*) REQVA) & PTE_P)
		panic("kpl ipcrecv: REQVA already mapped\n");
	
	do {
		r = ipc_recv(fsid, &from, (void*)REQVA, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	p = (struct Scfs_metadata*)REQVA;
	memcpy(md, (void*)REQVA, p->size + sizeof(size_t) + sizeof(id));
	sys_page_unmap(0, (void*)REQVA);
	return r;
}

int
cfs_set_metadata(const char *name, struct Scfs_metadata *md)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;
	struct Scfs_metadata *p;

	fsid = find_fs();

	struct Scfs_set_metadata *pg = (struct Scfs_set_metadata*)
		ROUNDUP32(ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_SET_METADATA;
	strncpy(pg->name, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	p = (struct Scfs_metadata*)pg;

	memcpy(p, md, sizeof(struct Scfs_metadata));

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	//if (get_pte((void*) REQVA) & PTE_P)
	//	panic("kpl ipcrecv: REQVA already mapped\n");
	
	do {
		r = ipc_recv(fsid, &from, 0, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	return r;
}

int
cfs_sync(const char *name)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_sync *pg = (struct Scfs_sync*)
		ROUNDUP32(ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_SYNC;
	if (name)
		strncpy(pg->name, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));
	else
		pg->name[0] = 0;

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	//if (get_pte((void*) REQVA) & PTE_P)
	//	panic("kpl ipcrecv: REQVA already mapped\n");
	
	do {
		r = ipc_recv(fsid, &from, 0, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	return r;
}

int
cfs_shutdown(void)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_shutdown *pg = (struct Scfs_shutdown*)
		ROUNDUP32(ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_SHUTDOWN;

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	//if (get_pte((void*) REQVA) & PTE_P)
	//	panic("kpl ipcrecv: REQVA already mapped\n");
	
	do {
		r = ipc_recv(fsid, &from, 0, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	return r;
}

int
cfs_debug(void)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_debug *pg = (struct Scfs_debug*)
		ROUNDUP32(ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_DEBUG;

	ipc_send(fsid, 0, pg, PTE_U|PTE_P, NULL);

	//if (get_pte((void*) REQVA) & PTE_P)
	//	panic("kpl ipcrecv: REQVA already mapped\n");
	
	do {
		r = ipc_recv(fsid, &from, 0, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("%s::ipc_recv\n", __FUNCTION__);
	} while (from != fsid);
	return r;
}
