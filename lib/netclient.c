#if defined(KUDOS)

#include "lwip/tcp.h"
#include <inc/net.h>
#include <inc/lib.h>
#include <lib/netclient.h>

const char netd_name_sh[]   = "/netd";
const char netd_name_kern[] = "netd";
const char netd_ipc_name_sh[]   = "/netd:IPC";
const char netd_ipc_name_kern[] = "netd:IPC";

static bool
env_is_netd_net(const struct Env *e)
{
	return (e->env_status != ENV_FREE
			&& ((!strncmp(e->env_name, netd_name_sh, strlen(netd_name_sh))
				 && e->env_name[strlen(netd_name_sh)] != ':')
				|| (!strncmp(e->env_name, netd_name_kern, strlen(netd_name_kern))
					&& e->env_name[strlen(netd_name_kern)] != ':')));
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

static envid_t
find_netd_net(void)
{
	size_t ntries;
	size_t i;

	// Try to find netd a few times, in case this env is being
	// started at the same time as netd, thus giving netd time to do its
	// fork.
	// 20 is most arbitrary: 10 worked in bochs, so I doubled to get 20.
	// NOTE: fsipc.c:find_fs() does the same.
	for (ntries = 0; ntries < 20; ntries++)
	{
		for (i = 0; i < NENV; i++)
		{
			if (env_is_netd_net(&envs[i]))
				return envs[i].env_id;
		}
		sys_yield();
	}

	return 0;
}


uint8_t req_buf[2*PGSIZE];

int
kgethostbyname(const char *name, struct ip_addr *ipaddr)
{
	const int name_len = strlen(name);
	int i, r;
	bool addr_name = 1; // 0 addr, 1 name
	envid_t netd_ipcrecv = 0, netd_net = 0;

	// Determine whether name is an ip address or hostname
	for (i=0; i < name_len; i++)
	{
		if ( (!isnum(name[i]) && ('.' != name[i])) || i > 4 )
		{
			addr_name = 1;
			break;
		}
		if ('.' == name[i])
		{
			addr_name = 0;
			break;
		}
	}

	if (addr_name == 0)
		return (kinet_atoip(name, ipaddr) == 1);

	netd_ipcrecv = find_netd_ipcrecv();
	if (!netd_ipcrecv)
	{
		kdprintf(STDERR_FILENO, "connect(): unable to find netd ipcrecv\n");
		return -1;
	}
	netd_net = find_netd_net();
	if (!netd_net)
	{
		kdprintf(STDERR_FILENO, "connect(): unable to find netd net\n");
		return -1;
	}

	// Setup lookup request
	struct Netreq_gethostbyname *req = (struct Netreq_gethostbyname*) ROUND32(req_buf, PGSIZE);
	strncpy(req->name, name, DNS_NAME_MAXLEN-1);

	// Send request
	ipc_send(netd_ipcrecv, NETREQ_GETHOSTBYNAME, req, PTE_P|PTE_U, NULL);

	// Determine whether the lookup succeded
	if ((r = (int32_t) ipc_recv(netd_net, NULL, NULL, NULL, NULL, 0)) < 0)
		return r;

	// Receive the ip address
	uint32_t ip = ipc_recv(netd_net, NULL, NULL, NULL, NULL, 0);
	*ipaddr = *(struct ip_addr*) &ip;

	return 0;
}

int
kconnect(struct ip_addr ipaddr, uint16_t port, int *fd)
{
	int r;
	envid_t netd_ipcrecv = 0, netd_net = 0;

	netd_ipcrecv = find_netd_ipcrecv();
	if (!netd_ipcrecv)
	{
		kdprintf(STDERR_FILENO, "connect(): unable to find netd ipcrecv\n");
		return -1;
	}
	netd_net = find_netd_net();
	if (!netd_net)
	{
		kdprintf(STDERR_FILENO, "connect(): unable to find netd net\n");
		return -1;
	}

	// Setup connect request
	struct Netreq_connect *req = (struct Netreq_connect*) ROUND32(req_buf, PGSIZE);
	req->req_ipaddr = ipaddr;
	req->req_port   = port;

	// Send request
	ipc_send(netd_ipcrecv, NETREQ_CONNECT, req, PTE_P|PTE_U, NULL);

	// Determine whether the connect succeded
	if ((r = (int32_t) ipc_recv(netd_net, NULL, NULL, NULL, NULL, 0)) < 0)
		return r;

	// Receive fds
	if ((*fd = r = dup2env_recv(netd_net)) < 0)
		panic("dup2env_recv: %e", r);

	return 0;
}

int
kbind_listen(struct ip_addr ipaddr, uint16_t port, uint32_t* listen_key)
{
	envid_t netd_ipcrecv = 0, netd_net = 0;
	int r;

	netd_ipcrecv = find_netd_ipcrecv();
	if (!netd_ipcrecv)
	{
		kdprintf(STDERR_FILENO, "bind_listen(): unable to find netd ipcrecv\n");
		return -1;
	}
	netd_net = find_netd_net();
	if (!netd_net)
	{
		kdprintf(STDERR_FILENO, "connect(): unable to find netd net\n");
		return -1;
	}

	// Setup bind_listen request
	struct Netreq_bind_listen *req = (struct Netreq_bind_listen*) ROUND32(req_buf, PGSIZE);
	req->req_ipaddr = ipaddr;
	req->req_port   = port;

	// Send request
	ipc_send(netd_ipcrecv, NETREQ_BIND_LISTEN, req, PTE_P|PTE_U, NULL);

	// Determine whether the bind_listen succeded
	if ((r = (int32_t) ipc_recv(netd_net, NULL, NULL, NULL, NULL, 0)) < 0)
		return r;

	// Receive listen key
	*listen_key = ipc_recv(netd_net, NULL, NULL, NULL, NULL, 0);

	return 0;
}

int
kclose_listen(uint32_t listen_key)
{
	panic("TODO?");
}

int
kaccept(uint32_t listen_key, int *fd, struct ip_addr* remote_ipaddr, uint16_t* remote_port)
{
	envid_t netd_ipcrecv = 0, netd_net = 0;
	struct ip_addr ripaddr;
	uint16_t rport;
	int r;

	netd_ipcrecv = find_netd_ipcrecv();
	if (!netd_ipcrecv)
	{
		kdprintf(STDERR_FILENO, "accept(): unable to find netd ipcrecv\n");
		return -1;
	}
	netd_net = find_netd_net();
	if (!netd_net)
	{
		kdprintf(STDERR_FILENO, "connect(): unable to find netd net\n");
		return -1;
	}

	// Setup accept request
	struct Netreq_accept *req = (struct Netreq_accept*) ROUND32(req_buf, PGSIZE);
	req->req_listen_key = listen_key;

	// Send request
	ipc_send(netd_ipcrecv, NETREQ_ACCEPT, req, PTE_P|PTE_U, NULL);

	// Determine whether the accept succeded
	if ((r = (int32_t) ipc_recv(netd_net, NULL, NULL, NULL, NULL, 0)) < 0)
		return r;

	// Receive the fds
	if ((*fd = r = dup2env_recv(netd_net)) < 0)
		panic("dup2env_recv: %e", r);

	// Receive the remote ipaddr and port
	ripaddr.addr = ipc_recv(netd_net, NULL, NULL, NULL, NULL, 0);
	rport = (uint16_t) ipc_recv(netd_net, NULL, NULL, NULL, NULL, 0);
	if (remote_ipaddr)
		*remote_ipaddr = ripaddr;
	if (remote_port)
		*remote_port = rport;

	return 0;
}

// Write netd stats to fd
int
knet_stats(int fd)
{
	envid_t netd_ipcrecv = 0, netd_net = 0;
	char stats_buf[128];
	int stats_fd;
	int n;
	int r;

	netd_ipcrecv = find_netd_ipcrecv();
	if (!netd_ipcrecv)
	{
		kdprintf(STDERR_FILENO, "net_stats: unable to find netd ipcrecv\n");
		return -1;
	}

	// stats_fd comes from a child of netd and we don't know their envid
	netd_net = 0;

	// Setup accept request
	struct Netreq_stats *req = (struct Netreq_stats*) ROUND32(req_buf, PGSIZE);

	// Send request
	ipc_send(netd_ipcrecv, NETREQ_STATS, req, PTE_P|PTE_U, NULL);

	// Receive stats fd
	if ((stats_fd = r = dup2env_recv(netd_net)) < 0)
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
		kdprintf(STDERR_FILENO, "close: %e\n", r);
		return -1;
	}

	return 0;
}


//
// ascii<->ip_addr functions
// These are not netclient specific, but this file is a good enough home.

int
kinet_atoip(const char* cp, struct ip_addr *addr)
{
	int r;
	struct in_addr in_addr;
	if((r = inet_aton(cp, &in_addr)) != 1)
		return r;
	addr->addr = in_addr.s_addr;
	return 1;
}

char*
kinet_iptoa(struct ip_addr addr)
{
	struct in_addr in_addr;
	in_addr.s_addr = addr.addr;
	return inet_ntoa(in_addr);
}

#elif defined(UNIXUSER)

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <inc/error.h>
#include <lib/kdprintf.h>
#include <lib/netclient.h>

int
kgethostbyname(const char * name, struct ip_addr * ipaddr)
{
	struct hostent * h = gethostbyname(name);
	if (!h)
	{
		switch (h_errno)
		{
			case HOST_NOT_FOUND:
				return -E_NOT_FOUND;
			case NO_ADDRESS:
				return -E_NOT_FOUND;
			case NO_RECOVERY: case TRY_AGAIN:
				return -E_UNSPECIFIED;
			default:
				assert(0);
		}
	}

	memcpy(&(ipaddr->sin_addr), h->h_addr_list[0], h->h_length);
	return 0;
}

int
kconnect(struct ip_addr ipaddr, uint16_t port, int *fd)
{
	struct sockaddr_in serv_addr;
	int sock;
	int r;

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_addr.s_addr = ipaddr.sin_addr.s_addr;
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(port);

	sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock == -1)
		return -E_UNSPECIFIED;

	r = connect(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
	if (r == -1)
	{
		(void) close(sock);
		return -E_UNSPECIFIED;
	}

	*fd = sock;
	return 0;
}

int
kbind_listen(struct ip_addr ipaddr, uint16_t port, uint32_t* listen_key)
{
	struct sockaddr_in listen_addr;
	const int backlog = 10; // TODO: what should this be?
	int sock;
	int r;

	memset(&listen_addr, 0, sizeof(listen_addr));
	listen_addr.sin_family = AF_INET;
	listen_addr.sin_port = port;
	listen_addr.sin_addr.s_addr = ((struct sockaddr_in) ipaddr).sin_addr.s_addr;

	sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock == -1)
		return -E_UNSPECIFIED;

	r = bind(sock, (struct sockaddr *) &listen_addr, sizeof(listen_addr));
	if (r == -1)
	{
		(void) close(sock);
		return -E_UNSPECIFIED;
	}

	r = listen(sock, backlog);
	if (r == -1)
	{
		(void) close(sock);
		if (errno == EADDRINUSE)
			return -E_NET_USE;
		else
			return -E_UNSPECIFIED;
	}

	*listen_key = sock;
	return 0;
}

int
kclose_listen(uint32_t listen_key)
{
	kdprintf(STDERR_FILENO, "TODO?");
	assert(0);
}

int
kaccept(uint32_t listen_key, int *fd, struct ip_addr* remote_ipaddr, uint16_t* remote_port)
{
	int r;

	do {
		r = accept(listen_key, NULL, NULL);
	} while  (r == -1 && errno == EINTR);
	
	if (r == -1)
	{
		if (errno == ECONNABORTED)
			return -E_NET_ABRT;
		else
			return -E_UNSPECIFIED;
	}

	*fd = listen_key;
	return 0;
}

int
knet_stats(int fd)
{
	assert(0); // currently, on uses in unix-user
}


int
kinet_atoip(const char* cp, struct ip_addr *addr)
{
	return inet_aton(cp, &(addr->sin_addr));
}

char*
kinet_iptoa(struct ip_addr addr)
{
	return inet_ntoa(addr.sin_addr);
}

#else
#error Unknown target system
#endif
