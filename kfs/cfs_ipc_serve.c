#include <lib/serial_cfs.h>
#include <kfs/cfs.h>
#include <kfs/kfsd.h>
#include <kfs/sync.h>
#include <kfs/opgroup.h>
#include <kfs/cfs_ipc_opgroup.h>
#include <kfs/cfs_ipc_serve.h>
#include <kfs/inodeman.h>
#include <kfs/table_classifier_cfs.h>

#include <inc/lib.h> // for get_pte()
#include <inc/env.h>
#include <stdlib.h>

#define CFS_IPC_SERVE_DEBUG 0


#if CFS_IPC_SERVE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


// Previous message store for two-part message methods.
// If prev_serve_recvs[i] == NULL, allocate it and then use it
// (this helps kfsd in bochs startup faster without much runtime overhead.)
struct prev_serve_recv {
	envid_t envid;
	int     type;
	uint8_t scfs[PGSIZE];
};
typedef struct prev_serve_recv prev_serve_recv_t;
static prev_serve_recv_t * prev_serve_recvs[NENV];


static CFS_t * frontend_cfs = NULL;

void set_frontend_cfs(CFS_t * cfs)
{
	frontend_cfs = cfs;
}

CFS_t * get_frontend_cfs(void)
{
	return frontend_cfs;
}


static const void * cur_page = NULL;

const void * cfs_ipc_serve_cur_page(void)
{
	return cur_page;
}


static envid_t cur_envid = 0;

envid_t cfs_ipc_serve_cur_envid(void)
{
	return cur_envid;
}


static uint32_t cur_cappa = 0;

uint32_t cfs_ipc_serve_cur_cappa(void)
{
	return cur_cappa;
}

void cfs_ipc_serve_set_cur_cappa(uint32_t x)
{
	cur_cappa = x;
}


static void cfs_ipc_serve_shutdown(void * arg)
{
	int i;

	if (frontend_cfs)
	{
		kfs_sync(INODE_NONE);
		DESTROY(frontend_cfs);
		frontend_cfs = NULL;
	}

	inodeman_shutdown();

	for (i = 0; i < sizeof(prev_serve_recvs)/(sizeof(prev_serve_recvs[0])); i++)
		free(prev_serve_recvs[i]);
}

// Return like a constructor would, 0 for fail
int cfs_ipc_serve_init(void)
{
	int r;

	if (get_pte((void*) PAGESNDVA) & PTE_P)
		panic("cfs_ipc_serve: PAGESNDVA already mapped");

	if ((r = inodeman_init()) < 0)
		return r;

	if ((r = kfsd_register_shutdown_module(cfs_ipc_serve_shutdown, NULL)) < 0)
		return r;
	return 1;
}

static void alloc_prevrecv(envid_t envid, prev_serve_recv_t ** prevrecv)
{
	*prevrecv = prev_serve_recvs[ENVX(envid)] = malloc(sizeof(prev_serve_recv_t));
	if (!*prevrecv)
	{
		kdprintf(STDERR_FILENO, "kfsd cfs_ipc_serve: malloc returned NULL\n");
		kfsd_shutdown();
	}
}


static void serve_open(envid_t envid, struct Scfs_open * req)
{
	bool new_file = 0;
	prev_serve_recv_t * prevrecv = prev_serve_recvs[ENVX(envid)];
	if (!prevrecv)
		alloc_prevrecv(envid, &prevrecv);

	if (!prevrecv->type || prevrecv->envid != envid)
	{
		// First of two recvs
		Dprintf("%s [1]: %08x, \"%s\", %d\n", __FUNCTION__, envid, req->path, req->mode);
		prevrecv->envid = envid;
		prevrecv->type = req->scfs_type;
		memcpy(prevrecv->scfs, req, PGSIZE);
	}
	else
	{
		// Second of two recvs
		struct Scfs_open *scfs = (struct Scfs_open*) prevrecv->scfs;
		CFS_t * select_cfs;
		int r;
		inode_t ino;
		Dprintf("%s [2]: %08x, \"%s\", %d\n", __FUNCTION__, envid, scfs->path, scfs->mode);
		cur_page = req;

		if (scfs->mode & O_CREAT)
		{
			inode_t parent;
			char * filename;
			r = path_to_parent_and_name(scfs->path, &select_cfs, &parent, &filename);
			if (r >= 0)
			{
				kfsd_set_mount(select_cfs);
				scfs->mode &= ~O_CREAT;
				r = CALL(frontend_cfs, lookup, parent, filename, &ino);
				if (r < 0)
				{
					new_file = 1;
					r = CALL(frontend_cfs, create, parent, filename, scfs->mode, &ino);
				}
			}
		}

		if (!new_file)
		{
			r = path_to_inode(scfs->path, &select_cfs, &ino);
			if (r >= 0)
			{
				kfsd_set_mount(select_cfs);
				r = CALL(frontend_cfs, open, ino, scfs->mode);
			}
		}

		cur_page = NULL;
		ipc_send(envid, r, NULL, 0, NULL);
		prevrecv->envid = 0;
		prevrecv->type  = 0;
	}
}

static void serve_close(envid_t envid, struct Scfs_close * req)
{
	int r;
	Dprintf("%s: %08x, %d\n", __FUNCTION__, envid, req->fid);
	r = CALL(frontend_cfs, close, req->fid);
	ipc_send(envid, r, NULL, 0, NULL);
}

static void serve_read(envid_t envid, struct Scfs_read * req)
{
	int r;
	void *buf = (uint8_t*) PAGESNDVA;
	Dprintf("%s: %08x, %d, %d, %d\n", __FUNCTION__, envid, req->fid, req->offset, req->size);

	if (get_pte(buf) & PTE_P)
		panic("buf (PAGESNDVA = 0x%08x) already mapped", buf);
	if ((r = sys_page_alloc(0, buf, PTE_P|PTE_U|PTE_W)) < 0)
		panic("sys_page_alloc: %e", r);
	r = CALL(frontend_cfs, read, req->fid, buf, req->offset, req->size);
	ipc_send(envid, r, buf, PTE_P|PTE_U, NULL);
}

static void serve_write(envid_t envid, struct Scfs_write * req)
{
	prev_serve_recv_t * prevrecv = prev_serve_recvs[ENVX(envid)];
	if (!prevrecv)
		alloc_prevrecv(envid, &prevrecv);

	if (!prevrecv->type || prevrecv->envid != envid)
	{
		// First of two recvs
		Dprintf("%s [1]: %08x, %d, %d, %d\n", __FUNCTION__, envid, req->fid, req->offset, req->size);
		prevrecv->envid = envid;
		prevrecv->type  = req->scfs_type;
		memcpy(prevrecv->scfs, req, PGSIZE);
	}
	else
	{
		// Second of two recvs
		struct Scfs_write *scfs = (struct Scfs_write*) prevrecv->scfs;
		int r;
		Dprintf("%s [2]: %08x, %d, %d, %d\n", __FUNCTION__, envid, scfs->fid, scfs->offset, scfs->size);
		r = CALL(frontend_cfs, write, scfs->fid, req, scfs->offset, scfs->size);
		ipc_send(envid, r, NULL, 0, NULL);
		prevrecv->envid = 0;
		prevrecv->type  = 0;
	}
}

static void serve_getdirentries(envid_t envid, struct Scfs_getdirentries * req)
{
	int nbytes;
	int r;
	struct Scfs_getdirentries_return * resp = (struct Scfs_getdirentries_return*) PAGESNDVA;
	Dprintf("%s: %08x, %d, %d\n", __FUNCTION__, envid, req->fid, req->basep);

	if (get_pte(resp) & PTE_P)
		panic("resp (PAGESNDVA = 0x%08x) already mapped", resp);
	if ((r = sys_page_alloc(0, resp, PTE_P|PTE_U|PTE_W)) < 0)
		panic("sys_page_alloc: %e", r);
	resp->basep = req->basep;
	nbytes = req->nbytes;
	if (nbytes > sizeof(resp->buf))
		nbytes = sizeof(resp->buf);

	r = CALL(frontend_cfs, getdirentries, req->fid, resp->buf, nbytes, &resp->basep);
	resp->nbytes_read = r;
	ipc_send(envid, r, resp, PTE_P|PTE_U, NULL);
}

static void serve_truncate(envid_t envid, struct Scfs_truncate * req)
{
	int r;
	Dprintf("%s: %08x, %d, %d\n", __FUNCTION__, envid, req->fid, req->size);
	r = CALL(frontend_cfs, truncate, req->fid, req->size);
	ipc_send(envid, r, NULL, 0, NULL);
}

static void serve_unlink(envid_t envid, struct Scfs_unlink * req)
{
	int r;
	inode_t parent;
	CFS_t * select_cfs;
	char * name;
	Dprintf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->name);
	r = path_to_parent_and_name(req->name, &select_cfs, &parent, &name);
	if (r >= 0) {
		kfsd_set_mount(select_cfs);
		r = CALL(frontend_cfs, unlink, parent, name);
	}
	ipc_send(envid, r, NULL, 0, NULL);
}

static void serve_link(envid_t envid, struct Scfs_link * req)
{
	int r;
	inode_t ino, newparent;
	CFS_t * select_cfs, * select_new_cfs;
	char * newname;
	Dprintf("%s: %08x, \"%s\", \"%s\"\n", __FUNCTION__, envid, req->oldname, req->newname);

	r = path_to_inode(req->oldname, &select_cfs, &ino);
	if (r >= 0) {
		r = path_to_parent_and_name(req->newname, &select_new_cfs, &newparent, &newname);
		if ((r >= 0) && (select_cfs == select_new_cfs)) {
			kfsd_set_mount(select_cfs);
			r = CALL(frontend_cfs, link, ino, newparent, newname);
		}
	}
	ipc_send(envid, r, NULL, 0, NULL);
}

static void serve_rename(envid_t envid, struct Scfs_rename * req)
{
	int r;
	inode_t oldparent, newparent;
	char * oldname, * newname;
	CFS_t * select_cfs, * select_new_cfs;
	Dprintf("%s: %08x, \"%s\", \"%s\"\n", __FUNCTION__, envid, req->oldname, req->newname);

	r = path_to_parent_and_name(req->oldname, &select_cfs, &oldparent, &oldname);
	if (r >= 0) {
		r = path_to_parent_and_name(req->newname, &select_new_cfs, &newparent, &newname);
		if ((r >= 0) && (select_cfs == select_new_cfs)) {
			kfsd_set_mount(select_cfs);
			r = CALL(frontend_cfs, rename, oldparent, oldname, newparent, newname);
		}
	}
	ipc_send(envid, r, NULL, 0, NULL);
}

static void serve_mkdir(envid_t envid, struct Scfs_mkdir * req)
{
	int r;
	inode_t ino, parent;
	char * name;
	CFS_t * select_cfs;
	Dprintf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->path);

	r = path_to_parent_and_name(req->path, &select_cfs, &parent, &name);
	if (r >= 0) {
		kfsd_set_mount(select_cfs);
		r = CALL(frontend_cfs, mkdir, parent, name, &ino);
	}
	ipc_send(envid, r, NULL, 0, NULL);
}

static void serve_rmdir(envid_t envid, struct Scfs_rmdir * req)
{
	int r;
	inode_t parent;
	char * name;
	CFS_t * select_cfs;
	Dprintf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->path);

	r = path_to_parent_and_name(req->path, &select_cfs, &parent, &name);
	if (r >= 0) {
		kfsd_set_mount(select_cfs);
		r = CALL(frontend_cfs, rmdir, parent, name);
	}
	ipc_send(envid, r, NULL, 0, NULL);
}

static void serve_get_num_features(envid_t envid, struct Scfs_get_num_features * req)
{
	int r;
	inode_t ino;
	CFS_t * select_cfs;
	Dprintf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->name);
	r = path_to_inode(req->name, &select_cfs, &ino);
	if (r >= 0) {
		kfsd_set_mount(select_cfs);
		r = CALL(frontend_cfs, get_num_features, ino);
	}
	ipc_send(envid, r, NULL, 0, NULL);
}

static void serve_get_feature(envid_t envid, struct Scfs_get_feature * req)
{
	const feature_t *f = NULL;
	void *buf = (uint8_t*) PAGESNDVA;
	int r;
	inode_t ino;
	CFS_t * select_cfs;
	Dprintf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->name);

	if (get_pte(buf) & PTE_P)
		panic("buf (PAGESNDVA = 0x%08x) already mapped", buf);
	if ((r = sys_page_alloc(0, buf, PTE_P|PTE_U|PTE_W)) < 0)
		panic("sys_page_alloc: %e", r);
	r = path_to_inode(req->name, &select_cfs, &ino);
	if (r >= 0) {
		kfsd_set_mount(select_cfs);
		f = CALL(frontend_cfs, get_feature, ino, req->num);
	}

	if (!f)
		ipc_send(envid, -E_UNSPECIFIED, NULL, 0, NULL);
	else
	{
		memcpy(buf, f, sizeof(*f));
		ipc_send(envid, r, buf, PTE_P|PTE_U, NULL);
	}
}

static void serve_get_metadata(envid_t envid, struct Scfs_get_metadata * req)
{
	struct Scfs_metadata *md = (struct Scfs_metadata*) PAGESNDVA;
	void * data = NULL;
	int r;
	inode_t ino;
	CFS_t * select_cfs;
	Dprintf("%s: %08x, \"%s\", %d\n", __FUNCTION__, envid, req->name, req->id);

	if (get_pte(md) & PTE_P)
		panic("md (PAGESNDVA = 0x%08x) already mapped", md);
	if ((r = sys_page_alloc(0, md, PTE_P|PTE_U|PTE_W)) < 0)
		panic("sys_page_alloc: %e", r);
	md->id = req->id;
	md->size = 0;

	r = path_to_inode(req->name, &select_cfs, &ino);
	if (r >= 0) {
		kfsd_set_mount(select_cfs);
		r = CALL(frontend_cfs, get_metadata, ino, req->id, &md->size, &data);
	}

	assert((md->size > 0 && data) || (!md->size && !data));
	if (data)
	{
		if (md->size > sizeof(md->data))
			kdprintf(STDERR_FILENO, "kfsd cfs_ipc_serve: CFS->get_metadata() returned more data (%d) than serial_cfs allows (%d), truncating.\n", md->size, sizeof(md->data));
		memcpy(md->data, data, MIN(md->size, sizeof(md->data)));
		free(data);
	}

	ipc_send(envid, r, (void*) md, PTE_P|PTE_U, NULL);
	sys_page_unmap(0, md);
}

static void serve_set_metadata(envid_t envid, struct Scfs_set_metadata * req)
{
	prev_serve_recv_t * prevrecv = prev_serve_recvs[ENVX(envid)];
	if (!prevrecv)
		alloc_prevrecv(envid, &prevrecv);

	if (!prevrecv->type || prevrecv->envid != envid)
	{
		// First of two recvs
		Dprintf("%s [1]: %08x, \"%s\"\n", __FUNCTION__, envid, req->name);
		prevrecv->envid = envid;
		prevrecv->type  = req->scfs_type;
		memcpy(prevrecv->scfs, req, PGSIZE);
	}
	else
	{
		// Second of two recvs
		struct Scfs_set_metadata *scfs = (struct Scfs_set_metadata*) prevrecv->scfs;
		struct Scfs_metadata *md = (struct Scfs_metadata*) req;
		CFS_t * select_cfs;
		int r;
		inode_t ino;
		Dprintf("%s [2]: %08x, \"%s\"\n", __FUNCTION__, envid, scfs->name);

		r = path_to_inode(scfs->name, &select_cfs, &ino);
		if (r >= 0) {
			kfsd_set_mount(select_cfs);
			r = CALL(frontend_cfs, set_metadata, ino, md->id, md->size, md->data);
		}
		ipc_send(envid, r, NULL, 0, NULL);
		prevrecv->envid = 0;
		prevrecv->type  = 0;
	}
}

static void serve_opgroup_scope_create(envid_t envid, struct Scfs_opgroup_scope_create * req)
{
	prev_serve_recv_t * prevrecv = prev_serve_recvs[ENVX(envid)];
	if (!prevrecv)
		alloc_prevrecv(envid, &prevrecv);

	if (!prevrecv->type || prevrecv->envid != envid)
	{
		// First of two recvs
		Dprintf("%s [1]: %08x, 0x%08x\n", __FUNCTION__, envid, req->scope_cappg_va);
		prevrecv->envid = envid;
		prevrecv->type = req->scfs_type;
		memcpy(prevrecv->scfs, req, PGSIZE);
	}
	else
	{
		// Second of two recvs
		struct Scfs_opgroup_scope_create *scfs = (struct Scfs_opgroup_scope_create*) prevrecv->scfs;
		int r;
		Dprintf("%s [2]: %08x, 0x%08x\n", __FUNCTION__, envid, scfs->scope_cappg_va);
		r = cfs_ipc_opgroup_scope_create(envid, req, scfs->scope_cappg_va);
		ipc_send(envid, r, NULL, 0, NULL);
		prevrecv->envid = 0;
		prevrecv->type = 0;
	}
}

static void serve_opgroup_scope_copy(envid_t envid, struct Scfs_opgroup_scope_copy * req)
{
	prev_serve_recv_t * prevrecv = prev_serve_recvs[ENVX(envid)];
	if (!prevrecv)
		alloc_prevrecv(envid, &prevrecv);

	if (!prevrecv->type || prevrecv->envid != envid)
	{
		// First of two recvs
		Dprintf("%s [1]: %08x, %08x, 0x%08x\n", __FUNCTION__, envid, req->child, req->child_scope_cappg_va);
		prevrecv->envid = envid;
		prevrecv->type = req->scfs_type;
		memcpy(prevrecv->scfs, req, PGSIZE);
	}
	else
	{
		// Second of two recvs
		struct Scfs_opgroup_scope_copy *scfs = (struct Scfs_opgroup_scope_copy*) prevrecv->scfs;
		int r;
		Dprintf("%s [2]: %08x, %08x, 0x%08x\n", __FUNCTION__, envid, scfs->child, scfs->child_scope_cappg_va);
		r = cfs_ipc_opgroup_scope_copy(envid, scfs->child, req, scfs->child_scope_cappg_va);
		ipc_send(envid, r, NULL, 0, NULL);
		prevrecv->envid = 0;
		prevrecv->type = 0;
	}
}

static void serve_opgroup_create(envid_t envid, struct Scfs_opgroup_create * req)
{
	Dprintf("%s: %08x, %d\n", __FUNCTION__, envid, req->flags);
	int opgroup = cfs_ipc_opgroup_create(envid, req->flags);
	ipc_send(envid, opgroup, NULL, 0, NULL);
}

static void serve_opgroup_add_depend(envid_t envid, struct Scfs_opgroup_add_depend * req)
{
	Dprintf("%s: %08x, %d, %d\n", __FUNCTION__, envid, req->dependent, req->dependency);
	int r = cfs_ipc_opgroup_add_depend(envid, req->dependent, req->dependency);
	ipc_send(envid, r, NULL, 0, NULL);
}

static void serve_opgroup_engage(envid_t envid, struct Scfs_opgroup_engage * req)
{
	Dprintf("%s: %08x, %d\n", __FUNCTION__, envid, req->opgroup);
	int r = cfs_ipc_opgroup_engage(envid, req->opgroup);
	ipc_send(envid, r, NULL, 0, NULL);
}

static void serve_opgroup_disengage(envid_t envid, struct Scfs_opgroup_disengage * req)
{
	Dprintf("%s: %08x, %d\n", __FUNCTION__, envid, req->opgroup);
	int r = cfs_ipc_opgroup_disengage(envid, req->opgroup);
	ipc_send(envid, r, NULL, 0, NULL);
}

static void serve_opgroup_release(envid_t envid, struct Scfs_opgroup_release * req)
{
	Dprintf("%s: %08x, %d\n", __FUNCTION__, envid, req->opgroup);
	int r = cfs_ipc_opgroup_release(envid, req->opgroup);
	ipc_send(envid, r, NULL, 0, NULL);
}

static void serve_opgroup_abandon(envid_t envid, struct Scfs_opgroup_abandon * req)
{
	Dprintf("%s: %08x, %d\n", __FUNCTION__, envid, req->opgroup);
	int r = cfs_ipc_opgroup_abandon(envid, req->opgroup);
	ipc_send(envid, r, NULL, 0, NULL);
}

static void serve_shutdown(envid_t envid, struct Scfs_shutdown * req)
{
	Dprintf("%s: %08x\n", __FUNCTION__, envid);
	/* must respond before shutdown, because kfsd_shutdown() exits */
	ipc_send(envid, 0, NULL, 0, NULL);
	kfsd_shutdown();
}

static void serve_debug(envid_t envid, struct Scfs_debug * req)
{
	Dprintf("%s: 0x%08x\n", __FUNCTION__, envid);
	malloc_stats();
	ipc_send(envid, 0, NULL, 0, NULL);
}


void cfs_ipc_serve_run(envid_t whom, void * pg, int perm, uint32_t cur_cappa)
{
	int type;
	int r;

	// All requests must contain an argument page
	if (! ((perm & PTE_P) && (perm & PTE_U)) )
	{
		kdprintf(STDERR_FILENO, "Invalid request from %08x: no argument page\n", whom);
		return; // just leave it hanging...
	}

	const prev_serve_recv_t * prevrecv = prev_serve_recvs[ENVX(whom)];
	if (prevrecv && prevrecv->type && prevrecv->envid == whom)
		type = prevrecv->type;
	else
		type = *((int*) pg);

	if (!frontend_cfs && type != SCFS_SHUTDOWN)
	{
		kdprintf(STDERR_FILENO, "kfsd cfs_ipc_serve: Received request but there is no registered frontend CFS object.\n");
		return; // just leave it hanging...
	}

	cur_envid = whom;

	switch (type) {
		case SCFS_OPEN:
			serve_open(whom, (struct Scfs_open*) pg);
			break;
		case SCFS_CLOSE:
			serve_close(whom, (struct Scfs_close*) pg);
			break;
		case SCFS_READ:
			serve_read(whom, (struct Scfs_read*) pg);
			break;
		case SCFS_WRITE:
			serve_write(whom, (struct Scfs_write*) pg);
			break;
		case SCFS_GETDIRENTRIES:
			serve_getdirentries(whom, (struct Scfs_getdirentries*) pg);
			break;
		case SCFS_TRUNCATE:
			serve_truncate(whom, (struct Scfs_truncate*) pg);
			break;
		case SCFS_UNLINK:
			serve_unlink(whom, (struct Scfs_unlink*) pg);
			break;
		case SCFS_LINK:
			serve_link(whom, (struct Scfs_link*) pg);
			break;
		case SCFS_RENAME:
			serve_rename(whom, (struct Scfs_rename*) pg);
			break;
		case SCFS_MKDIR:
			serve_mkdir(whom, (struct Scfs_mkdir*) pg);
			break;
		case SCFS_RMDIR:
			serve_rmdir(whom, (struct Scfs_rmdir*) pg);
			break;
		case SCFS_GET_NUM_FEATURES:
			serve_get_num_features(whom, (struct Scfs_get_num_features*) pg);
			break;
		case SCFS_GET_FEATURE:
			serve_get_feature(whom, (struct Scfs_get_feature*) pg);
			break;
		case SCFS_GET_METADATA:
			serve_get_metadata(whom, (struct Scfs_get_metadata*) pg);
			break;
		case SCFS_SET_METADATA:
			serve_set_metadata(whom, (struct Scfs_set_metadata*) pg);
			break;
		case SCFS_OPGROUP_SCOPE_CREATE:
			serve_opgroup_scope_create(whom, (struct Scfs_opgroup_scope_create*) pg);
			break;
		case SCFS_OPGROUP_SCOPE_COPY:
			serve_opgroup_scope_copy(whom, (struct Scfs_opgroup_scope_copy*) pg);
			break;
		case SCFS_OPGROUP_CREATE:
			serve_opgroup_create(whom, (struct Scfs_opgroup_create*) pg);
			break;
		case SCFS_OPGROUP_ADD_DEPEND:
			serve_opgroup_add_depend(whom, (struct Scfs_opgroup_add_depend*) pg);
			break;
		case SCFS_OPGROUP_ENGAGE:
			serve_opgroup_engage(whom, (struct Scfs_opgroup_engage*) pg);
			break;
		case SCFS_OPGROUP_DISENGAGE:
			serve_opgroup_disengage(whom, (struct Scfs_opgroup_disengage*) pg);
			break;
		case SCFS_OPGROUP_RELEASE:
			serve_opgroup_release(whom, (struct Scfs_opgroup_release*) pg);
			break;
		case SCFS_OPGROUP_ABANDON:
			serve_opgroup_abandon(whom, (struct Scfs_opgroup_abandon*) pg);
			break;
		case SCFS_SHUTDOWN:
			serve_shutdown(whom, (struct Scfs_shutdown*) pg);
			break;
		case SCFS_DEBUG:
			serve_debug(whom, (struct Scfs_debug*) pg);
			break;
		default:
			kdprintf(STDERR_FILENO, "kfsd cfs_ipc_serve: Unknown type %d\n", type);
	}

	cur_envid = 0;

	if ((r = sys_page_unmap(0, (void*) PAGESNDVA)) < 0)
		panic("sys_page_unmap: %e", r);
	cur_cappa = 0;
}
