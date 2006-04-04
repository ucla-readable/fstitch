#include <inc/lib.h>
#include <lib/serial_cfs.h>
#include <inc/cfs_ipc_client.h>

#include <kfs/opgroup.h>

opgroup_id_t
opgroup_create(int flags)
{
	envid_t fsid;
	int r;

	fsid = find_fs();

	if((r = cfs_ensure_opgroup_scope_exists(fsid)) < 0)
		return r;

	struct Scfs_opgroup_create *pg = (struct Scfs_opgroup_create*)
		ROUNDUP32(__cfs_ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_OPGROUP_CREATE;
	pg->flags = flags;

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, NULL, NULL, 0);
}

int
opgroup_sync(opgroup_id_t opgroup)
{
	panic("opgroup_sync() not yet implemented for KudOS");
}

int
opgroup_add_depend(opgroup_id_t dependent, opgroup_id_t dependency)
{
	envid_t fsid;

	fsid = find_fs();

	assert(cfs_opgroup_scope_exists());

	struct Scfs_opgroup_add_depend *pg = (struct Scfs_opgroup_add_depend*)
		ROUNDUP32(__cfs_ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_OPGROUP_ADD_DEPEND;
	pg->dependent = dependent;
	pg->dependency = dependency;

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, NULL, NULL, 0);
}

int
opgroup_engage(opgroup_id_t opgroup)
{
	envid_t fsid;

	fsid = find_fs();

	assert(cfs_opgroup_scope_exists());

	struct Scfs_opgroup_engage *pg = (struct Scfs_opgroup_engage*)
		ROUNDUP32(__cfs_ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_OPGROUP_ENGAGE;
	pg->opgroup = opgroup;

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, NULL, NULL, 0);
}

int
opgroup_disengage(opgroup_id_t opgroup)
{
	envid_t fsid;

	fsid = find_fs();

	assert(cfs_opgroup_scope_exists());

	struct Scfs_opgroup_disengage *pg = (struct Scfs_opgroup_disengage*)
		ROUNDUP32(__cfs_ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_OPGROUP_DISENGAGE;
	pg->opgroup = opgroup;

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, NULL, NULL, 0);
}

int
opgroup_release(opgroup_id_t opgroup)
{
	envid_t fsid;

	fsid = find_fs();

	assert(cfs_opgroup_scope_exists());

	struct Scfs_opgroup_release *pg = (struct Scfs_opgroup_release*)
		ROUNDUP32(__cfs_ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_OPGROUP_RELEASE;
	pg->opgroup = opgroup;

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, NULL, NULL, 0);
}

int
opgroup_abandon(opgroup_id_t opgroup)
{
	envid_t fsid;

	fsid = find_fs();

	assert(cfs_opgroup_scope_exists());

	struct Scfs_opgroup_abandon *pg = (struct Scfs_opgroup_abandon*)
		ROUNDUP32(__cfs_ipc_page, PGSIZE);
	memset(pg, 0, PGSIZE);
	pg->scfs_type = SCFS_OPGROUP_ABANDON;
	pg->opgroup = opgroup;

	ipc_send(fsid, SCFS_VAL, pg, PTE_U|PTE_P, NULL);

	return ipc_recv(fsid, NULL, 0, NULL, NULL, 0);
}
