#include <inc/lib.h>
#include <lib/serial_cfs.h>
#include <inc/cfs_ipc_client.h>

#include <kfs/opgroup.h>

static const char kfsd_name_sh[] = "/kfsd";
static const char kfsd_name_kern[] = "kfsd";

envid_t
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
#define OPGROUPSCOPE_CAPPG ((void *) (REQVA - PGSIZE))
#define OPGROUPSCOPE_CHILD_CAPPG (OPGROUPSCOPE_CAPPG - PGSIZE)

uint8_t __cfs_ipc_page[PGSIZE] __attribute__((__aligned__(PGSIZE)));
#define ipc_page __cfs_ipc_page


static int
cfs_opgroup_scope_create(envid_t fsid)
{
	int r;

	if(get_pte(OPGROUPSCOPE_CAPPG) & PTE_P)
		return -E_UNSPECIFIED;

	if(!(fsid = find_fs()))
		return -E_TIMEOUT;

	if((r = sys_page_alloc(0, OPGROUPSCOPE_CAPPG, PTE_U|PTE_P|PTE_SHARE)) < 0)
		return r;
	struct Scfs_opgroup_scope_create *pg = (struct Scfs_opgroup_scope_create*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_OPGROUP_SCOPE_CREATE;
	pg->scope_cappg_va = (uintptr_t) OPGROUPSCOPE_CAPPG;

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, OPGROUPSCOPE_CAPPG);

	ipc_send(fsid, SCFS_VAL, OPGROUPSCOPE_CAPPG, PTE_U|PTE_P, OPGROUPSCOPE_CAPPG);

	r = ipc_recv(fsid, NULL, 0, NULL, NULL, 0);
	if (r < 0)
		(void) sys_page_unmap(0, OPGROUPSCOPE_CAPPG);
	return r;
}

int
cfs_opgroup_scope_copy(envid_t child)
{
	envid_t fsid;
	int r, s;

	// No copy needed if this env does not have an opgroup scope
	if(!(get_pte(OPGROUPSCOPE_CAPPG) & PTE_P))
		return 0;

	// While the above cappg check may return 0 from within kfsd,
	// do a second, name, check as well just in case kfsd does fork()/spawn()
	if(!strcmp(env->env_name, "kfsd"))
		return 0;

	if(!(fsid = find_fs()))
		return -E_TIMEOUT;

	// Create a cappg for the child
	if((r = sys_page_alloc(child, OPGROUPSCOPE_CAPPG, PTE_U|PTE_P|PTE_SHARE)) < 0)
		return r;

	// Map the child's cappg into us so that we can send it to kfsd
	// on the child's behalf
	r = sys_page_map(child, OPGROUPSCOPE_CAPPG, 0, OPGROUPSCOPE_CHILD_CAPPG, PTE_U|PTE_P|PTE_SHARE);
	if(r < 0)
	{
		(void) sys_page_unmap(child, OPGROUPSCOPE_CAPPG);
		return r;
	}

	struct Scfs_opgroup_scope_copy *pg = (struct Scfs_opgroup_scope_copy*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_OPGROUP_SCOPE_COPY;
	pg->child = child;
	pg->child_scope_cappg_va = (uintptr_t) OPGROUPSCOPE_CAPPG;

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, OPGROUPSCOPE_CHILD_CAPPG);

	ipc_send(fsid, SCFS_VAL, OPGROUPSCOPE_CHILD_CAPPG, PTE_U|PTE_P, OPGROUPSCOPE_CHILD_CAPPG);

	r = ipc_recv(fsid, NULL, 0, NULL, NULL, 0);
	if (r < 0)
		(void) sys_page_unmap(child, OPGROUPSCOPE_CAPPG);

	s = sys_page_unmap(0, OPGROUPSCOPE_CHILD_CAPPG);
	assert(r < 0 || s >= 0); // hard to recover from and shouldn't happen..

	return r;
}

int
cfs_ensure_opgroup_scope_exists(envid_t fsid)
{
	if(get_pte(OPGROUPSCOPE_CAPPG) & PTE_P)
		return 0;

	return cfs_opgroup_scope_create(fsid);
}


/* copy a path into the request, prepending a / in case the client did not have one */
/* we do this here instead of in KPL because we are already copying the string here */
static void
cfs_pathcpy(char *dst, const char *src, size_t len)
{
	if(*src != '/')
	{
		*(dst++) = '/';
		len--;
	}
	strncpy(dst, src, len);
}


//
// Serial CFS

int
cfs_open(const char *fname, int mode, void *refpg, const void * cappg)
{
	envid_t fsid;
	uint32_t perm;
	int r;

	fsid = find_fs();

	if((r = cfs_ensure_opgroup_scope_exists(fsid)) < 0)
		return r;

	struct Scfs_open *pg = (struct Scfs_open*) ipc_page;
   	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_OPEN;
	pg->mode = mode;
	cfs_pathcpy(pg->path, fname, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, cappg);

	ipc_send(fsid, SCFS_VAL, refpg, PTE_U|PTE_P, cappg);

	return ipc_recv(fsid, NULL, 0, &perm, NULL, 0);
}

int
cfs_close(int fid, const void * cappg)
{
	envid_t fsid;
	uint32_t perm;

	assert(get_pte((void*) OPGROUPSCOPE_CAPPG) & PTE_P);

	fsid = find_fs();

	struct Scfs_close *pg = (struct Scfs_close*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_CLOSE;
	pg->fid = fid;

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, cappg);

	//assert(!(get_pte((void*) REQVA) & PTE_P)); // FIXME: why does this fail?
	return ipc_recv(fsid, NULL, 0, &perm, NULL, 0);
}

int
cfs_read(int fid, uint32_t offset, uint32_t size, char *data, const void * cappg)
{
	int r = 0;
	envid_t fsid;
	uint32_t perm;
	uint32_t i;

	assert(get_pte((void*) OPGROUPSCOPE_CAPPG) & PTE_P);

	fsid = find_fs();

	for (i = 0; i < size; i += PGSIZE) {
		struct Scfs_read *pg = (struct Scfs_read*) ipc_page;
		const uint32_t requested = MIN(size - i, PGSIZE);

		memset(pg, 0, PGSIZE);
		pg->scfs_type = SCFS_READ;
		pg->fid = fid;
		pg->offset = offset + i;
		pg->size = requested;

		ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, cappg);

		assert(!(get_pte((void*) REQVA) & PTE_P));
		r = ipc_recv(fsid, NULL, (void*)REQVA, &perm, NULL, 0);
		if (r < 0)
		{
			sys_page_unmap(0, (void*)REQVA);
			return i;
		}
		memcpy(data + i, (void*)REQVA, requested);
		sys_page_unmap(0, (void*)REQVA);
		if (r < requested)
			return i + r;
	}
	return size;
}

int
cfs_write(int fid, uint32_t offset, uint32_t size, const char *data, const void * cappg)
{
	int r = 0;
	envid_t fsid;
	uint32_t perm;
	uint32_t i;

	assert(get_pte((void*) OPGROUPSCOPE_CAPPG) & PTE_P);

	fsid = find_fs();

	for (i = 0; i < size; i += PGSIZE) {
		struct Scfs_write *pg = (struct Scfs_write*) ipc_page;
		memset(pg, 0, PGSIZE);
		pg->scfs_type = SCFS_WRITE;
		pg->fid = fid;
		pg->offset = offset + i;
		pg->size = MIN(size - i, PGSIZE);

		ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, cappg);
		r = sys_page_unmap(0, pg);
		if (r < 0) panic("%s:%d\n", __FILE__, __LINE__);
		r = sys_page_alloc(0, pg, PTE_W|PTE_U|PTE_P);
		if (r < 0) panic("%s:%d\n", __FILE__, __LINE__);

		memcpy(pg, data + i, MIN(size - i, PGSIZE));

		ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, cappg);

		r = ipc_recv(fsid, NULL, 0, &perm, NULL, 0);
		/* have to cast to int for signed comparisons */
		if (r < (int) MIN(size - i, PGSIZE))
			return i ? ((r < 0) ? i : i + r) : r;
	}
	return size;
}

int
cfs_getdirentries(int fid, char * buf, size_t nbytes, off_t *basep, const void * cappg)
{
	int r = 0, nbytes_read = 0;
	envid_t fsid;
	uint32_t perm;
	struct Scfs_getdirentries_return *ret = (struct Scfs_getdirentries_return*) REQVA;

	assert(get_pte((void*) OPGROUPSCOPE_CAPPG) & PTE_P);

	fsid = find_fs();

	struct Scfs_getdirentries *pg = (struct Scfs_getdirentries*) ipc_page;
	while (nbytes_read < nbytes)
	{
		memset(pg, 0, PGSIZE);
		pg->scfs_type = SCFS_GETDIRENTRIES;
		pg->fid = fid;
		pg->nbytes = nbytes - nbytes_read;
		pg->basep = *basep;

		ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, cappg);
		
		assert(!(get_pte((void*) ret) & PTE_P));
		r = ipc_recv(fsid, NULL, (void*) ret, &perm, NULL, 0);
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
cfs_truncate(int fid, uint32_t size, const void * cappg)
{
	envid_t fsid;
	uint32_t perm;

	assert(get_pte((void*) OPGROUPSCOPE_CAPPG) & PTE_P);

	fsid = find_fs();

	struct Scfs_truncate *pg = (struct Scfs_truncate*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_TRUNCATE;
	pg->fid = fid;
	pg->size = size;

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, cappg);

	return ipc_recv(fsid, NULL, 0, &perm, NULL, 0);
}

int
cfs_unlink(const char *name)
{
	envid_t fsid;
	uint32_t perm;
	int r;

	fsid = find_fs();

	if((r = cfs_ensure_opgroup_scope_exists(fsid)) < 0)
		return r;

	struct Scfs_unlink *pg = (struct Scfs_unlink*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_UNLINK;
	cfs_pathcpy(pg->name, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, &perm, NULL, 0);
}

int
cfs_link(const char *oldname, const char *newname)
{
	envid_t fsid;
	uint32_t perm;
	int r;

	fsid = find_fs();

	if((r = cfs_ensure_opgroup_scope_exists(fsid)) < 0)
		return r;

	struct Scfs_link *pg = (struct Scfs_link*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_LINK;
	cfs_pathcpy(pg->oldname, oldname, MIN(SCFSMAXNAMELEN, MAXNAMELEN));
	cfs_pathcpy(pg->newname, newname, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, &perm, NULL, 0);
}

int
cfs_rename(const char *oldname, const char *newname)
{
	envid_t fsid;
	uint32_t perm;
	int r;

	fsid = find_fs();

	if((r = cfs_ensure_opgroup_scope_exists(fsid)) < 0)
		return r;

	struct Scfs_rename *pg = (struct Scfs_rename*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_RENAME;
	cfs_pathcpy(pg->oldname, oldname, MIN(SCFSMAXNAMELEN, MAXNAMELEN));
	cfs_pathcpy(pg->newname, newname, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, &perm, NULL, 0);
}

int
cfs_mkdir(const char *name)
{
	envid_t fsid;
	uint32_t perm;
	int r;

	fsid = find_fs();

	if((r = cfs_ensure_opgroup_scope_exists(fsid)) < 0)
		return r;

	struct Scfs_mkdir *pg = (struct Scfs_mkdir*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_MKDIR;
	cfs_pathcpy(pg->path, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, &perm, NULL, 0);
}

int
cfs_rmdir(const char *name)
{
	envid_t fsid;
	uint32_t perm;
	int r;

	fsid = find_fs();

	if((r = cfs_ensure_opgroup_scope_exists(fsid)) < 0)
		return r;

	struct Scfs_rmdir *pg = (struct Scfs_rmdir*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_RMDIR;
	cfs_pathcpy(pg->path, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, &perm, NULL, 0);
}

int
cfs_get_num_features(char *name)
{
	envid_t fsid;
	uint32_t perm;
	int r;

	fsid = find_fs();

	if((r = cfs_ensure_opgroup_scope_exists(fsid)) < 0)
		return r;

	struct Scfs_get_num_features *pg = (struct Scfs_get_num_features*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_GET_NUM_FEATURES;
	cfs_pathcpy(pg->name, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, &perm, NULL, 0);
}

int
cfs_get_feature(char *name, int num, char *dump)
{
	int r;
	envid_t fsid;
	uint32_t perm;

	fsid = find_fs();

	if((r = cfs_ensure_opgroup_scope_exists(fsid)) < 0)
		return r;

	struct Scfs_get_feature *pg = (struct Scfs_get_feature*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_GET_FEATURE;
	cfs_pathcpy(pg->name, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));
	pg->num = num;

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	assert(!(get_pte((void*) REQVA) & PTE_P));	
	r = ipc_recv(fsid, NULL, (void*)REQVA, &perm, NULL, 0);
	memcpy(dump, (void*)REQVA, PGSIZE);
	sys_page_unmap(0, (void*)REQVA);
	return r;
}

int
cfs_get_metadata(const char *name, int id, struct Scfs_metadata *md)
{
	int r;
	envid_t fsid;
	uint32_t perm;
	struct Scfs_metadata *p;

	fsid = find_fs();

	if((r = cfs_ensure_opgroup_scope_exists(fsid)) < 0)
		return r;

	struct Scfs_get_metadata *pg = (struct Scfs_get_metadata*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_GET_METADATA;
	pg->id = id;
	cfs_pathcpy(pg->name, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	assert(!(get_pte((void*) REQVA) & PTE_P));	
	r = ipc_recv(fsid, NULL, (void*)REQVA, &perm, NULL, 0);
	p = (struct Scfs_metadata*)REQVA;
	memcpy(md, (void*)REQVA, p->size + sizeof(size_t) + sizeof(id));
	sys_page_unmap(0, (void*)REQVA);
	return r;
}

int
cfs_set_metadata(const char *name, struct Scfs_metadata *md)
{
	envid_t fsid;
	uint32_t perm;
	struct Scfs_metadata *p;
	int r;

	fsid = find_fs();

	if((r = cfs_ensure_opgroup_scope_exists(fsid)) < 0)
		return r;

	struct Scfs_set_metadata *pg = (struct Scfs_set_metadata*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_SET_METADATA;
	cfs_pathcpy(pg->name, name, MIN(SCFSMAXNAMELEN, MAXNAMELEN));

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	p = (struct Scfs_metadata*)pg;

	memcpy(p, md, sizeof(struct Scfs_metadata));

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, &perm, NULL, 0);
}


int
cfs_shutdown(void)
{
	envid_t fsid;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_shutdown *pg = (struct Scfs_shutdown*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_SHUTDOWN;

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, &perm, NULL, 0);
}

int
cfs_debug(void)
{
	envid_t fsid;
	uint32_t perm;

	fsid = find_fs();

	struct Scfs_debug *pg = (struct Scfs_debug*) ipc_page;
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_DEBUG;

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, &perm, NULL, 0);
}
