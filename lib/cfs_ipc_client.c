#include <inc/lib.h>
#include <inc/serial_cfs.h>

#include "cfs_ipc_client.h"

const char kfsd_name_sh[] = "/kfsd";
const char kfsd_name_kern[] = "kfsd";

static envid_t
find_fs()
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
cfs_open(char *fname, int mode, void *refpg)
{
	int r;
	envid_t fsid;
	envid_t from;
	uint32_t perm;

	fsid = find_fs();
	printf("sending to env: %08x\n", fsid);

	struct Scfs_open *pg = (struct Scfs_open*)
		ROUNDUP32(ipc_page, PGSIZE);
   	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_OPEN;
	pg->mode = mode;
	strcpy(pg->path, fname);

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	ipc_send(fsid, 0, refpg, PTE_U|PTE_P);

	//if (get_pte((void*) REQVA) & PTE_P)
	//	panic("kpl ipcrecv: REQVA already mapped\n");

	do {
		r = ipc_recv(&from, 0, &perm, 0);
		assert(from == fsid);
		if (from == 0) panic("cfs_open::ipc_recv\n");
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

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	//if (get_pte((void*) REQVA) & PTE_P)
	//	panic("kpl ipcrecv: REQVA already mapped\n");

	do {
		r = ipc_recv(&from, 0, &perm, 0);
		if (from == 0) panic("cfs_close::ipc_recv\n");
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

		ipc_send(fsid, 0, pg, PTE_U|PTE_P);

		if (get_pte((void*) REQVA) & PTE_P)
			panic("kpl ipcrecv: REQVA already mapped\n");

		do {
			r = ipc_recv(&from, (void*)REQVA, &perm, 0);
			if (from == 0) panic("cfs_read::ipc_recv\n");
		} while (from != fsid);
		if (r < 0) return r;
		memcpy(data + i, (void*)REQVA, MIN(size-i, PGSIZE));
		sys_page_unmap(0, (void*)REQVA);
	}
	return r;
}

int
cfs_write(int fid, uint32_t offset, uint32_t size, char *data)
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

		ipc_send(fsid, 0, pg, PTE_U|PTE_P);
		r = sys_page_unmap(0, pg);
		if (r < 0) panic("%s:%d\n", __FILE__, __LINE__);
		r = sys_page_alloc(0, pg, PTE_W|PTE_U|PTE_P);
		if (r < 0) panic("%s:%d\n", __FILE__, __LINE__);

		memcpy(pg, data + i, MIN(size - i, PGSIZE));

		ipc_send(fsid, 0, pg, PTE_U|PTE_P);

		do {
			r = ipc_recv(&from, 0, &perm, 0);
			if (from == 0) panic("cfs_write::ipc_recv\n");
		} while (from != fsid);
	}
	return r;
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

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	do {
		r = ipc_recv(&from, 0, &perm, 0);
		if (from == 0) panic("cfs_truncate::ipc_recv\n");
	} while (from != fsid);
	return r;
}

int
cfs_unlink(char *name)
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
	strcpy(pg->name, name);

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	do {
		r = ipc_recv(&from, 0, &perm, 0);
		if (from == 0) panic("cfs_unlink::ipc_recv\n");
	} while (from != fsid);
	return r;
}

int
cfs_link(char *oldname, char *newname)
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
	strcpy(pg->oldname, oldname);
	strcpy(pg->newname, newname);

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	do {
		r = ipc_recv(&from, 0, &perm, 0);
		if (from == 0) panic("cfs_link::ipc_recv\n");
	} while (from != fsid);
	return r;
}

int
cfs_rename(char *oldname, char *newname)
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
	strcpy(pg->oldname, oldname);
	strcpy(pg->newname, newname);

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	do {
		r = ipc_recv(&from, 0, &perm, 0);
		if (from == 0) panic("cfs_rename::ipc_recv\n");
	} while (from != fsid);
	return r;
}

int
cfs_mkdir(char *name)
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
	strcpy(pg->path, name);

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	do {
		r = ipc_recv(&from, 0, &perm, 0);
		if (from == 0) panic("cfs_mkdir::ipc_recv\n");
	} while (from != fsid);
	return r;
}

int
cfs_rmdir(char *name)
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
	strcpy(pg->path, name);

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	do {
		r = ipc_recv(&from, 0, &perm, 0);
		if (from == 0) panic("cfs_rmdir::ipc_recv\n");
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
	strcpy(pg->name, name);

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	do {
		r = ipc_recv(&from, 0, &perm, 0);
		if (from == 0) panic("cfs_get_features::ipc_recv\n");
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
	strcpy(pg->name, name);
	pg->num = num;

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	if (get_pte((void*) REQVA) & PTE_P)
		panic("kpl ipcrecv: REQVA already mapped\n");
	
	do {
		r = ipc_recv(&from, (void*)REQVA, &perm, 0);
		if (from == 0) panic("cfs_get_features::ipc_recv\n");
	} while (from != fsid);
	memcpy(dump, (void*)REQVA, PGSIZE);
	sys_page_unmap(0, (void*)REQVA);
	return r;
}

int
cfs_get_metadata(char *name, int id, struct Scfs_metadata *md)
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
	strcpy(pg->name, name);

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	if (get_pte((void*) REQVA) & PTE_P)
		panic("kpl ipcrecv: REQVA already mapped\n");
	
	do {
		r = ipc_recv(&from, (void*)REQVA, &perm, 0);
		if (from == 0) panic("cfs_get_metadata::ipc_recv\n");
	} while (from != fsid);
	p = (struct Scfs_metadata*)REQVA;
	memcpy(md, (void*)REQVA, p->size + sizeof(size_t) + sizeof(id));
	sys_page_unmap(0, (void*)REQVA);
	return r;
}

int
cfs_set_metadata(char *name, struct Scfs_metadata *md)
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
	strcpy(pg->name, name);

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	p = (struct Scfs_metadata*)pg;

	memcpy(p, md, sizeof(struct Scfs_metadata));

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	//if (get_pte((void*) REQVA) & PTE_P)
	//	panic("kpl ipcrecv: REQVA already mapped\n");
	
	do {
		r = ipc_recv(&from, 0, &perm, 0);
		if (from == 0) panic("cfs_get_metadata::ipc_recv\n");
	} while (from != fsid);
	return r;
}

int
cfs_sync(char *name)
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
	strcpy(pg->name, name);

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	//if (get_pte((void*) REQVA) & PTE_P)
	//	panic("kpl ipcrecv: REQVA already mapped\n");
	
	do {
		r = ipc_recv(&from, 0, &perm, 0);
		if (from == 0) panic("cfs_get_metadata::ipc_recv\n");
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

	ipc_send(fsid, 0, pg, PTE_U|PTE_P);

	//if (get_pte((void*) REQVA) & PTE_P)
	//	panic("kpl ipcrecv: REQVA already mapped\n");
	
	do {
		r = ipc_recv(&from, 0, &perm, 0);
		if (from == 0) panic("cfs_get_metadata::ipc_recv\n");
	} while (from != fsid);
	return r;
}
