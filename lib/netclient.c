#include "lwip/tcp.h"
#include <inc/net.h>
#include <inc/lib.h>

const char netd_name_sh[]   = "/netd";
const char netd_name_kern[] = "netd";
const char netd_ipc_name_sh[]   = "/netd:IPC";
const char netd_ipc_name_kern[] = "netd:IPC";

static bool
env_is_netd_net(const struct Env *e)
{
	return (e->env_status != ENV_FREE &&
			  (!strncmp(e->env_name, netd_name_sh, strlen(netd_name_sh))
				|| !strncmp(e->env_name, netd_name_kern, strlen(netd_name_kern))));
}

static bool
env_is_netd_ipcrecv(const struct Env *e)
{
	return (e->env_status != ENV_FREE
			  && strstr(e->env_name, ":IPC")
			  && env_is_netd_net(&envs[ENVX(e->env_parent_id)]));
}

static envid_t
find_netd_ipcrecv(void)
{
	size_t ntries;
	size_t i;

	// Try to find netc ipcrecv a few times, in case this env is being
	// started at the same time as netd, thus giving netd time to do its
	// fork.
	// 20 is most arbitrary: 10 worked in bochs, so I doubled to get 20.
	// NOTE: fsipc.c:find_fs() does the same.
	for (ntries = 0; ntries < 20; ntries++)
	{
		for (i = 0; i < NENV; i++)
		{
			if (env_is_netd_ipcrecv(&envs[i]))
				return envs[i].env_id;
		}
		sys_yield();
	}

	return 0;
}


uint8_t req_buf[2*PGSIZE];

int
connect(struct ip_addr ipaddr, uint16_t port, int fd[2])
{
	int r;
	envid_t netd_ipcrecv = 0;

	netd_ipcrecv = find_netd_ipcrecv();
	if (!netd_ipcrecv)
	{
		fprintf(STDERR_FILENO, "connect(): unable to find netd ipcrecv\n");
		return -1;
	}

	// Setup connect request
	struct Netreq_connect *req = (struct Netreq_connect*) ROUND32(req_buf, PGSIZE);
	req->req_ipaddr = ipaddr;
	req->req_port   = port;

	// Send request
	ipc_send(netd_ipcrecv, NETREQ_CONNECT, req, PTE_P|PTE_U);

	// Determine whether the connect succeded
	if ((r = (int32_t) ipc_recv(NULL, NULL, NULL, 0)) < 0)
		return r;

	// Receive fds
	if ((fd[0] = r = dup2env_recv()) < 0)
		panic("dup2env_recv: %e", r);
	if ((fd[1] = dup2env_recv()) < 0)
		panic("dup2env_recv: %e", r);

	return 0;
}

int
bind_listen(struct ip_addr ipaddr, uint16_t port, uint32_t* listen_key)
{
	envid_t netd_ipcrecv;
	int r;

	netd_ipcrecv = find_netd_ipcrecv();
	if (!netd_ipcrecv)
	{
		fprintf(STDERR_FILENO, "bind_listen(): unable to find netd ipcrecv\n");
		return -1;
	}

	// Setup bind_listen request
	struct Netreq_bind_listen *req = (struct Netreq_bind_listen*) ROUND32(req_buf, PGSIZE);
	req->req_ipaddr = ipaddr;
	req->req_port   = port;

	// Send request
	ipc_send(netd_ipcrecv, NETREQ_BIND_LISTEN, req, PTE_P|PTE_U);

	// Determine whether the bind_listen succeded
	if ((r = (int32_t) ipc_recv(NULL, NULL, NULL, 0)) < 0)
		return r;

	// Receive listen key
	*listen_key = ipc_recv(NULL, NULL, NULL, 0);

	return 0;
}

int
close_listen(uint32_t listen_key)
{
	panic("TODO?");
}

int
accept(uint32_t listen_key, int fd[2], struct ip_addr* remote_ipaddr, uint16_t* remote_port)
{
	envid_t netd_ipcrecv;
	struct ip_addr ripaddr;
	uint16_t rport;
	int r;

	netd_ipcrecv = find_netd_ipcrecv();
	if (!netd_ipcrecv)
	{
		fprintf(STDERR_FILENO, "accept(): unable to find netd ipcrecv\n");
		return -1;
	}

	// Setup accept request
	struct Netreq_accept *req = (struct Netreq_accept*) ROUND32(req_buf, PGSIZE);
	req->req_listen_key = listen_key;

	// Send request
	ipc_send(netd_ipcrecv, NETREQ_ACCEPT, req, PTE_P|PTE_U);

	// Determine whether the accept succeded
	if ((r = (int32_t) ipc_recv(NULL, NULL, NULL, 0)) < 0)
		return r;

	// Receive the fds
	if ((fd[0] = r = dup2env_recv()) < 0)
		panic("dup2env_recv: %e", r);
	if ((fd[1] = dup2env_recv()) < 0)
		panic("dup2env_recv: %e", r);

	// Receive the remote ipaddr and port
	ripaddr.addr = ipc_recv(NULL, NULL, NULL, 0);
	rport = (uint16_t) ipc_recv(NULL, NULL, NULL, 0);
	if (remote_ipaddr)
		*remote_ipaddr = ripaddr;
	if (remote_port)
		*remote_port = rport;

	return 0;
}

// Write netd stats to fd
int
net_stats(int fd)
{
	envid_t netd_ipcrecv;
	char stats_buf[128];
	int stats_fd;
	int n;
	int r;

	netd_ipcrecv = find_netd_ipcrecv();
	if (!netd_ipcrecv)
	{
		fprintf(STDERR_FILENO, "net_stats: unable to find netd ipcrecv\n");
		return -1;
	}

	// Setup accept request
	struct Netreq_stats *req = (struct Netreq_stats*) ROUND32(req_buf, PGSIZE);

	// Send request
	ipc_send(netd_ipcrecv, NETREQ_STATS, req, PTE_P|PTE_U);

	// Receive stats fd
	if ((stats_fd = r = dup2env_recv()) < 0)
		panic("dup2env_recv: %e", r);

	// Copy data from stats_fd to fd
	while ((n = read(stats_fd, stats_buf, sizeof(stats_buf))) > 0)
	{
		if ((r = write(fd, stats_buf, n)) < 0)
			panic("write: %e", r);
		if (n != r)
			panic("n (%d) != r (%d)", n, r);
	}

	if ((r = close(stats_fd)) < 0)
	{
		fprintf(STDERR_FILENO, "close: %e\n", r);
		return -1;
	}

	return 0;
}


//
// ascii<->ip_addr functions
// These are not netclient specific, but this file is a good enough home.

int
inet_atoip(const char* cp, struct ip_addr *addr)
{
	int r;
	struct in_addr in_addr;
	if((r = inet_aton(cp, &in_addr)) != 1)
		return r;
	addr->addr = in_addr.s_addr;
	return 1;
}

char*
inet_iptoa(struct ip_addr addr)
{
	struct in_addr in_addr;
	in_addr.s_addr = addr.addr;
	return inet_ntoa(in_addr);
}
