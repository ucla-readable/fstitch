#include <kfs/cfs.h>
#include <inc/serial_cfs.h>
#include <kfs/kfsd.h>
#include <kfs/cfs_ipc_serve.h>

#include <inc/lib.h> // for get_pte()
#include <inc/env.h>

#define IPC_RECV_TIMEOUT 100

#define CFS_IPC_SERVE_DEBUG 1


#if CFS_IPC_SERVE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

// Va at which to receive page mappings containing client reqs.
// This is the same va as serv.c's, why not.
#define REQVA (0x10000000 - PGSIZE)
#define PAGESNDVA (REQVA - PGSIZE)

struct prev_serve_recv {
	envid_t envid;
	int     type;
	uint8_t scfs[PGSIZE];
};
static struct prev_serve_recv prev_serve_recvs[NENV];

static void serve();


static CFS_t * frontend_cfs = NULL;

int register_frontend_cfs(CFS_t * cfs)
{
	frontend_cfs = cfs;
	return 0;
}


static void cfs_ipc_serve_shutdown(void * arg)
{
	if (frontend_cfs)
	{
		DESTROY(frontend_cfs);
		frontend_cfs = NULL;
	}
}

// Return like a constructor would, 0 for fail
int cfs_ipc_serve()
{
	int r;

	if (get_pte((void*) REQVA) & PTE_P)
		panic("cfs_ipc_serve: REQVA already mapped");
	if (get_pte((void*) PAGESNDVA) & PTE_P)
		panic("cfs_ipc_serve: PAGESNDVA already mapped");

	if ((r = kfsd_register_shutdown_module(cfs_ipc_serve_shutdown, NULL)) < 0)
		return r;
	return 1;
}

void cfs_ipc_serve_run()
{
	serve();
}


static void serve_open(envid_t envid, struct Scfs_open * req)
{
	Dprintf("%s: %08x, \"%s\", %d\n", __FUNCTION__, envid, req->path, req->mode);
	int r;
	r = CALL(frontend_cfs, open, req->path, req->mode);
	ipc_send(envid, r, NULL, 0);
}

static void serve_close(envid_t envid, struct Scfs_close * req)
{
	Dprintf("%s: %08x, %d\n", __FUNCTION__, envid, req->fid);
	int r;
	r = CALL(frontend_cfs, close, req->fid);
	ipc_send(envid, r, NULL, 0);
}

static void serve_read(envid_t envid, struct Scfs_read * req)
{
	Dprintf("%s: %08x, %d, %d, %d\n", __FUNCTION__, envid, req->fid, req->offset, req->size);
	int r;
	void *buf = (uint8_t*) PAGESNDVA;
	if ((r = sys_page_alloc(0, buf, PTE_P|PTE_U|PTE_W)) < 0)
		panic("sys_page_alloc: %e", r);
	r = CALL(frontend_cfs, read, req->fid, buf, req->offset, req->size);
	ipc_send(envid, r, buf, PTE_P|PTE_U);
}

static void serve_write(envid_t envid, struct Scfs_write * req)
{
	struct prev_serve_recv *prevrecv = &prev_serve_recvs[ENVX(envid)];
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
		Dprintf("%s [2]: %08x, %d, %d, %d\n", __FUNCTION__, envid, req->fid, req->offset, req->size);
		struct Scfs_write *scfs = (struct Scfs_write*) prevrecv->scfs;
		int r;
		r = CALL(frontend_cfs, write, scfs->fid, req, scfs->offset, scfs->size);
		ipc_send(envid, r, NULL, 0);
		prevrecv->envid = 0;
		prevrecv->type  = 0;
	}
}

static void serve_truncate(envid_t envid, struct Scfs_truncate * req)
{
	Dprintf("%s: %08x, %d, %d\n", __FUNCTION__, envid, req->fid, req->size);
	int r;
	r = CALL(frontend_cfs, truncate, req->fid, req->size);
	ipc_send(envid, r, NULL, 0);
}

static void serve_unlink(envid_t envid, struct Scfs_unlink * req)
{
	Dprintf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->name);
	int r;
	r = CALL(frontend_cfs, unlink, req->name);
	ipc_send(envid, r, NULL, 0);
}

static void serve_link(envid_t envid, struct Scfs_link * req)
{
	Dprintf("%s: %08x, \"%s\", \"%s\"\n", __FUNCTION__, envid, req->oldname, req->newname);
	int r;
	r = CALL(frontend_cfs, link, req->oldname, req->newname);
	ipc_send(envid, r, NULL, 0);
}

static void serve_rename(envid_t envid, struct Scfs_rename * req)
{
	Dprintf("%s: %08x, \"%s\", \"%s\"\n", __FUNCTION__, envid, req->oldname, req->newname);
	int r;
	r = CALL(frontend_cfs, rename, req->oldname, req->newname);
	ipc_send(envid, r, NULL, 0);
}

static void serve_mkdir(envid_t envid, struct Scfs_mkdir * req)
{
	Dprintf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->path);
	int r;
	r = CALL(frontend_cfs, mkdir, req->path);
	ipc_send(envid, r, NULL, 0);
}

static void serve_rmdir(envid_t envid, struct Scfs_rmdir * req)
{
	Dprintf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->path);
	int r;
	r = CALL(frontend_cfs, rmdir, req->path);
	ipc_send(envid, r, NULL, 0);
}

static void serve_get_num_features(envid_t envid, struct Scfs_get_num_features * req)
{
	Dprintf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->name);
	int r;
	r = CALL(frontend_cfs, get_num_features, req->name);
	ipc_send(envid, r, NULL, 0);
}

static void serve_get_feature(envid_t envid, struct Scfs_get_feature * req)
{
	Dprintf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->name);
	const feature_t *f;
	void *buf = (uint8_t*) PAGESNDVA;
	int r;

	if ((r = sys_page_alloc(0, buf, PTE_P|PTE_U|PTE_W)) < 0)
		panic("sys_page_alloc: %e", r);
	f = CALL(frontend_cfs, get_feature, req->name, req->num);
	if (!f)
		ipc_send(envid, -E_UNSPECIFIED, (void*) NULL, 0);
	else
	{
		memcpy(buf, f, sizeof(*f));
		ipc_send(envid, r, buf, PTE_P|PTE_U);
	}
}

static void serve_get_metadata(envid_t envid, struct Scfs_get_metadata * req)
{
	Dprintf("%s: %08x, \"%s\", %d\n", __FUNCTION__, envid, req->name, req->id);
	struct Scfs_metadata *md = (struct Scfs_metadata*) PAGESNDVA;
	int r;

	if ((r = sys_page_alloc(0, md, PTE_P|PTE_U|PTE_W)) < 0)
		panic("sys_page_alloc: %e", r);
	md->id = req->id;
	r = CALL(frontend_cfs, get_metadata, req->name, req->id, &md->size, md->data);
	ipc_send(envid, r, (void*) md, PTE_P|PTE_U);
}

static void serve_set_metadata(envid_t envid, struct Scfs_set_metadata * req)
{
	struct prev_serve_recv *prevrecv = &prev_serve_recvs[ENVX(envid)];
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
		Dprintf("%s [2]: %08x, \"%s\"\n", __FUNCTION__, envid, req->name);
		struct Scfs_set_metadata *scfs = (struct Scfs_set_metadata*) prevrecv->scfs;
		struct Scfs_metadata *md = (struct Scfs_metadata*) req;
		int r;
		r = CALL(frontend_cfs, set_metadata, scfs->name, md->id, md->size, md->data);
		ipc_send(envid, r, NULL, 0);
		prevrecv->envid = 0;
		prevrecv->type  = 0;
	}
}

static void serve_sync(envid_t envid, struct Scfs_sync * req)
{
	Dprintf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->name);
	int r;
	r = CALL(frontend_cfs, sync, req->name);
	ipc_send(envid, r, NULL, 0);
}

static void serve_shutdown(envid_t envid, struct Scfs_shutdown * req)
{
	Dprintf("%s: %08x\n", __FUNCTION__, envid);
	ipc_send(envid, 0, NULL, 0);
	kfsd_shutdown();
}


static void serve()
{
	uint32_t whom;
	int type;
	int perm = 0;
	uint32_t r;

	r = ipc_recv(&whom, (void*) REQVA, &perm, IPC_RECV_TIMEOUT);
	if (!whom && !perm)
	{
		if (r == -E_TIMEOUT)
			return;
		else
		{
			fprintf(STDERR_FILENO, "kfsd %s:%s: ipc_recv: %e\n", __FILE__, __FUNCTION__, (int) r);
			return;
		}
	}

	// All requests must contain an argument page
	if ((!perm & PTE_P))
	{
		fprintf(STDERR_FILENO, "Invalid request from %08x: no argument page\n", whom);
		return; // just leave it hanging...
	}

	const struct prev_serve_recv *prevrecv = &prev_serve_recvs[ENVX(whom)];
	if (prevrecv->type && prevrecv->envid == whom)
		type = prevrecv->type;
	else
		type = *((int*) REQVA);

	switch (type) {
		case SCFS_OPEN:
			serve_open(whom, (struct Scfs_open*) REQVA);
			break;
		case SCFS_CLOSE:
			serve_close(whom, (struct Scfs_close*) REQVA);
			break;
		case SCFS_READ:
			serve_read(whom, (struct Scfs_read*) REQVA);
			break;
		case SCFS_WRITE:
			serve_write(whom, (struct Scfs_write*) REQVA);
			break;
		case SCFS_TRUNCATE:
			serve_truncate(whom, (struct Scfs_truncate*) REQVA);
			break;
		case SCFS_UNLINK:
			serve_unlink(whom, (struct Scfs_unlink*) REQVA);
			break;
		case SCFS_LINK:
			serve_link(whom, (struct Scfs_link*) REQVA);
			break;
		case SCFS_RENAME:
			serve_rename(whom, (struct Scfs_rename*) REQVA);
			break;
		case SCFS_MKDIR:
			serve_mkdir(whom, (struct Scfs_mkdir*) REQVA);
			break;
		case SCFS_RMDIR:
			serve_rmdir(whom, (struct Scfs_rmdir*) REQVA);
			break;
		case SCFS_GET_NUM_FEATURES:
			serve_get_num_features(whom, (struct Scfs_get_num_features*) REQVA);
			break;
		case SCFS_GET_FEATURE:
			serve_get_feature(whom, (struct Scfs_get_feature*) REQVA);
			break;
		case SCFS_GET_METADATA:
			serve_get_metadata(whom, (struct Scfs_get_metadata*) REQVA);
			break;
		case SCFS_SET_METADATA:
			serve_set_metadata(whom, (struct Scfs_set_metadata*) REQVA);
			break;
		case SCFS_SYNC:
			serve_sync(whom, (struct Scfs_sync*) REQVA);
			break;
		case SCFS_SHUTDOWN:
			serve_shutdown(whom, (struct Scfs_shutdown*) REQVA);
			break;
		default:
			fprintf(STDERR_FILENO, "kfsd %s: Unknown type %d\n", __FUNCTION__, type);
	}
	if ((r = sys_page_unmap(0, (void*) REQVA)) < 0)
		panic("sys_page_unmap: %e", r);
	if ((r = sys_page_unmap(0, (void*) PAGESNDVA)) < 0)
		panic("sys_page_unmap: %e", r);
}
