#include <kfs/cfs.h>
#include <inc/serial_cfs.h>
#include <kfs/kfsd.h>
#include <kfs/cfs_ipc_serve.h>

#include <inc/lib.h> // for get_pte()
#include <inc/env.h>

// Va at which to receive page mappings containing client reqs.
// This is the same va as serv.c's, why not.
#define REQVA (0x10000000 - PGSIZE)

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
		panic("cfsipc: REQVA already mapped");

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
	printf("%s: %08x, \"%s\", %d\n", __FUNCTION__, envid, req->path, req->mode);
	ipc_send(envid, 0, NULL, 0);
}

static void serve_close(envid_t envid, struct Scfs_close * req)
{
	printf("%s: %08x, %d\n", __FUNCTION__, envid, req->fid);
	ipc_send(envid, 0, NULL, 0);
}

static void serve_read(envid_t envid, struct Scfs_read * req)
{
	printf("%s: %08x, %d, %d, %d\n", __FUNCTION__, envid, req->fid, req->offset, req->size);
	ipc_send(envid, 0, (void*) "0123456789", PTE_P|PTE_U);
}

static void serve_write(envid_t envid, struct Scfs_write * req)
{
	struct prev_serve_recv *prevrecv = &prev_serve_recvs[ENVX(envid)];
	if (prevrecv->envid == envid && !prevrecv->type)
	{
		// First of two recvs
		prevrecv->envid = envid;
		prevrecv->type  = req->scfs_type;
		memcpy(prevrecv->scfs, req, PGSIZE);

		printf("%s [1]: %08x, %d, %d, %d\n", __FUNCTION__, envid, req->fid, req->offset, req->size);
	}
	else
	{
		// Second of two recvs
		ipc_send(envid, 0, NULL, 0);
		printf("%s [2]: %08x, %d, %d, %d\n", __FUNCTION__, envid, req->fid, req->offset, req->size);
		prevrecv->envid = 0;
		prevrecv->type  = 0;
	}
}

static void serve_truncate(envid_t envid, struct Scfs_truncate * req)
{
	printf("%s: %08x, %d, %d\n", __FUNCTION__, envid, req->fid, req->size);
	ipc_send(envid, 0, NULL, 0);
}

static void serve_unlink(envid_t envid, struct Scfs_unlink * req)
{
	printf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->name);
	ipc_send(envid, 0, NULL, 0);
}

static void serve_link(envid_t envid, struct Scfs_link * req)
{
	printf("%s: %08x, \"%s\", \"%s\"\n", __FUNCTION__, envid, req->oldname, req->newname);
	ipc_send(envid, 0, NULL, 0);
}

static void serve_rename(envid_t envid, struct Scfs_rename * req)
{
	printf("%s: %08x, \"%s\", \"%s\"\n", __FUNCTION__, envid, req->oldname, req->newname);
	ipc_send(envid, 0, NULL, 0);
}

static void serve_mkdir(envid_t envid, struct Scfs_mkdir * req)
{
	printf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->path);
	ipc_send(envid, 0, NULL, 0);
}

static void serve_rmdir(envid_t envid, struct Scfs_rmdir * req)
{
	printf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->path);
	ipc_send(envid, 0, NULL, 0);
}

static void serve_get_features(envid_t envid, struct Scfs_get_features * req)
{
	printf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->name);
	ipc_send(envid, 0, (void*) REQVA, PTE_P|PTE_U);
}

static void serve_get_metadata(envid_t envid, struct Scfs_get_metadata * req)
{
	printf("%s: %08x, \"%s\", %d\n", __FUNCTION__, envid, req->name, req->id);
	ipc_send(envid, 0, (void*) REQVA, PTE_P|PTE_U);
}

static void serve_set_metadata(envid_t envid, struct Scfs_set_metadata * req)
{
	struct prev_serve_recv *prevrecv = &prev_serve_recvs[ENVX(envid)];
	if (prevrecv->envid == envid && !prevrecv->type)
	{
		// First of two recvs
		prevrecv->envid = envid;
		prevrecv->type  = req->scfs_type;
		memcpy(prevrecv->scfs, req, PGSIZE);

		printf("%s [1]: %08x, \"%s\"\n", __FUNCTION__, envid, req->name);
	}
	else
	{
		// Second of two recvs
		printf("%s [2]: %08x, \"%s\"\n", __FUNCTION__, envid, req->name);
		ipc_send(envid, 0, NULL, 0);
		prevrecv->envid = 0;
		prevrecv->type  = 0;
	}
}

static void serve_sync(envid_t envid, struct Scfs_sync * req)
{
	printf("%s: %08x, \"%s\"\n", __FUNCTION__, envid, req->name);
	ipc_send(envid, 0, NULL, 0);
}

static void serve_shutdown(envid_t envid, struct Scfs_shutdown * req)
{
	printf("%s: %08x\n", __FUNCTION__, envid);
	ipc_send(envid, 0, NULL, 0);
	kfsd_shutdown();
}


static void serve()
{
	uint32_t whom;
	int type;
	int perm = 0;
	uint32_t r;

	r = ipc_recv(&whom, (void*) REQVA, &perm, 0);
	if (!whom && !perm)
	{
		fprintf(STDERR_FILENO, "kfsd %s:%s: ipc_recv: %e\n", __FILE__, __FUNCTION__, (int) r);
		return;
	}

	// All requests must contain an argument page
	if ((!perm & PTE_P))
	{
		fprintf(STDERR_FILENO, "Invalid request from %08x: no argument page\n", whom);
		return; // just leave it hanging...
	}

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
		case SCFS_GET_FEATURES:
			serve_get_features(whom, (struct Scfs_get_features*) REQVA);
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
	}
	sys_page_unmap(0, (void*) REQVA);
}
