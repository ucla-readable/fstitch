//
// netd - Network Daemon

// TODO:
// - See if we can only write to pipes that have free space (see TODO)
// - Support more than one bind_listen() in an environment
// - Optimize buffer sizes/poll period for speed

#include <inc/lib.h>
#include <inc/net.h>
#include <inc/malloc.h>

#include "lwip/debug.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"
#include "arch/simple.h"


#define DEBUG_CONNSTATUS (1<<2)
#define DEBUG_REQ        (1<<3)
#define DEBUG_IPCRECV    (1<<4)

bool quiet = 0;
int  debug = 0;

//
// The netd network process

struct listen_state {
	struct tcp_pcb *pcb;
	envid_t acceptor;
	envid_t listener;
	struct ip_addr ipaddr;
	uint16_t port;
};

// Statically allocate these so that we can find expired listens
struct listen_state listen_states[NENV];

// This value should probably be about the size you find you need pipes, to
// have to get good throughput.
#define PER_TCP_PCB_BUFFER (16*PGSIZE)

struct buf {
	uint8_t *data;
	uint8_t  _data[PER_TCP_PCB_BUFFER];
	uint32_t left;
	uint8_t  retries;
};

struct client_state {
	int to_client, from_client;
	envid_t envid; // known to be true only until connected
	bool eof;
	struct buf send_buf;
};


static void
listen_state_init(struct listen_state *ls)
{
	ls->pcb      = NULL;
	ls->acceptor = 0;
	ls->listener = 0;
	ls->ipaddr   = ip_addr_any;
	ls->port     = 0;
}

static void
buf_init(struct buf *b)
{
	b->data    = b->_data;
	b->left    = 0;
	b->retries = 0;
}

static void
client_state_init(struct client_state *cs)
{
	cs->to_client   = -1;
	cs->from_client = -1;
	cs->envid       = 0;
	cs->eof         = 0;

	buf_init(&cs->send_buf);
}

static void
gc_listens()
{
	err_t err;
	int i;

	for (i = 0; i < NENV; i++)
	{
		if (listen_states[i].pcb
			 && (envs[i].env_id != listen_states[i].listener
				  || envs[i].env_status == ENV_FREE))
		{
			if ((err = tcp_close(listen_states[i].pcb)) != ERR_OK)
			{
				fprintf(STDERR_FILENO, "netd gc_listens: tcp_close: \"%s\", aborting.\n", lwip_strerr(err));
				tcp_abort(listen_states[i].pcb);
			}
			listen_states[i].pcb = NULL;
		}
	}
}

/*---------------------------------------------------------------------------*/
static void
setup_client_netd_pipes(envid_t client, int *to_client, int *from_client)
{
	int to_client_tmp[2];
	int from_client_tmp[2];
	int r;
	
	if ((r = pipe(to_client_tmp)) < 0)
		panic("pipe: %e", r);
	if ((r = pipe(from_client_tmp)) < 0)
		panic("pipe: %e", r);
	*to_client  = to_client_tmp[1];
	*from_client = from_client_tmp[0];
	if ((r = dup2env_send(to_client_tmp[0], client)) < 0)
		panic("dup2env_send: %e", r);
	if ((r = dup2env_send(from_client_tmp[1], client)) < 0)
		panic("dup2env_send: %e", r);
	if ((r = close(to_client_tmp[0])) < 0)
		panic("close: %e", r);
	if ((r = close(from_client_tmp[1])) < 0)
		panic("close: %e", r);
}
/*---------------------------------------------------------------------------*/
static void
close_conn(struct tcp_pcb *pcb, struct client_state *cs, int netclient_err)
{
	err_t err;
	int r;

	if (!cs->eof)
	{
		// Client did not close the connection, the other end caused it.
		// At least for now, do nothing special.
	}

	// Notify client of closing
	if (cs->to_client == -1 && cs->from_client == -1)
	{
		// Error while connecting/accepting.
		// Inform client of failure (client is ipc_recv()ing)
		ipc_send(cs->envid, netclient_err, NULL, 0);
	}
	else
	{
		// Error while connected.
		// Inform client of failure only by closing the pipes
		if ((r = close(cs->to_client)) < 0)
			fprintf(STDERR_FILENO, "WARNING: netd: close: %e\n", r);
		if ((r = close(cs->from_client)) < 0)
			fprintf(STDERR_FILENO, "WARNING: netd: close: %e\n", r);
	}

	// Deallocate resources
	if (pcb)
	{
		tcp_arg(pcb, NULL);
		tcp_poll(pcb, NULL, 0);
		tcp_accept(pcb, NULL);
		tcp_sent(pcb, NULL);
		tcp_recv(pcb, NULL);
		tcp_err(pcb, NULL);
	}

	if (debug & DEBUG_CONNSTATUS)
	{
		printf("netd connection closed %s:%d",
				 inet_iptoa(pcb->local_ip), (int) pcb->local_port);
		printf("<->%s:%d\n",
				 inet_iptoa(pcb->remote_ip), (int) pcb->remote_port);
	}

	if (pcb)
	{
		if ((err = tcp_close(pcb)) != ERR_OK)
		{
			fprintf(STDERR_FILENO, "netd close_conn: tcp_close: \"%s\", aborting.\n", lwip_strerr(err));
			tcp_abort(pcb);
		}
	}

	free(cs);
}

static int
lwip_to_netclient_err(err_t err)
{
	switch (err)
	{
		case ERR_OK:
			return 0;
		case ERR_MEM:
			return -E_NO_MEM;
		case ERR_ABRT:
			return -E_NET_ABRT;
		case ERR_RST:
			return -E_NET_RST;
		case ERR_CONN:
			return -E_NET_CONN;
		case ERR_USE:
			return -E_NET_USE;
		case ERR_IF:
			return -E_NET_IF;
		default:
			fprintf(STDERR_FILENO, "netd Connection closed with lwip err %d is %s. TODO: translate to netclient error code.\n", err, lwip_strerr(err));
			return -E_UNSPECIFIED;
	}
}

static void
conn_err_client(void *arg, err_t err)
{
	struct client_state *cs;

	cs = arg;

	if (ERR_ABRT == err && !cs)
	{
		// Normal, the connection was safely closed.
	}
	else
	{
		// TODO: can we get pcb so that tcp_close(pcb) is done?
		close_conn(NULL, cs, lwip_to_netclient_err(err));
	}
}

static void
conn_err_listen(void *arg, err_t err)
{
	struct listen_state *ls;

	ls = arg;

	if (ls->acceptor)
	{
		// Inform acceptor of failure (client is ipc_recv()ing)
		ipc_send(ls->acceptor, lwip_to_netclient_err(err), NULL, 0);
	}
	else
	{
		fprintf(STDERR_FILENO, "netd listen error on %s:%d, no acceptor: %s\n",
				  inet_iptoa(ls->ipaddr), ls->port, lwip_strerr(err));
	}

	if (debug & DEBUG_CONNSTATUS)
		printf("netd listen on %s:%d closed, err = %s\n",
				 inet_iptoa(ls->pcb->remote_ip), ls->pcb->remote_port, lwip_strerr(err));

	if (ls->pcb)
	{
		if ((err = tcp_close(ls->pcb)) != ERR_OK)
		{
			fprintf(STDERR_FILENO, "netd close_err_listen: tcp_close: %s, aborting.\n", lwip_strerr(err));
			tcp_abort(ls->pcb);
		}
	}

	// Mark ls as not in use
	ls->pcb = NULL;
}

static void
send_data(struct tcp_pcb *pcb, struct client_state *cs)
{
	err_t err;
	u16_t len;

	/* We cannot send more data than space available in the send buffer. */
	if (tcp_sndbuf(pcb) < cs->send_buf.left)
		len = tcp_sndbuf(pcb);
	else
		len = cs->send_buf.left;
	
	do
	{
		// If you are read page faulting, uncomment the the below to
		// make panic if send_data() is passed unmapped data:
		/*
		if (!(get_pte(cs->send_buf.data) & PTE_P))
			panic("Read page fault avoided: cs = 0x%x, get_pte(cs->send_buf.data = 0x%x) = 0x%x. cs->send_buf.left = %d\n", cs, cs->send_buf.data, get_pte(cs->send_buf.data), cs->send_buf.left);
		*/

		if ((err = tcp_write(pcb, cs->send_buf.data, len, 0)) == ERR_MEM)
			len /= 2;
	} while (err == ERR_MEM && len > 1);
  
	if (err == ERR_OK)
	{
		cs->send_buf.data += len;
		cs->send_buf.left -= len;
	}
	else
		fprintf(STDERR_FILENO, "netd send_data: error %s len %d %d\n", lwip_strerr(err), len, tcp_sndbuf(pcb));
}

static err_t
netd_queue_send(struct client_state *cs, struct tcp_pcb *pcb)
{
	if (cs->send_buf.left != 0)
		return ERR_OK;

	//
	// If client has closed pipe, close the connection

	if (cs->eof)
	{
		close_conn(pcb, cs, ERR_OK);
		return ERR_OK;
	}
		
	//
	// Otherwise, the connection is still active
	// Send any data from the client

	cs->send_buf.data = cs->send_buf._data;
	cs->send_buf.left = 0;
	int n = 0;
	do
	{
		n = read_nb(cs->from_client, cs->send_buf.data, sizeof(cs->send_buf._data) - cs->send_buf.left);
		
		if (n == -1)
			break;
	
		if (n == 0 && (cs->send_buf.left != sizeof(cs->send_buf._data)))
			cs->eof = 1;
	
		cs->send_buf.data += n;
		cs->send_buf.left += n;
	} while (n != 0);
		
	if (cs->send_buf.left > 0)
	{
		cs->send_buf.data = cs->send_buf._data;
		send_data(pcb, cs);
	}

	return ERR_OK;
}

static err_t
netd_poll(void *arg, struct tcp_pcb *pcb)
{
	struct client_state *cs;
	err_t err;

	cs = arg;
	
	if (cs == NULL)
	{
		if ((err = tcp_close(pcb)) != ERR_OK)
		{
			fprintf(STDERR_FILENO, "netd netd_poll: tcp_close: %s, aborting.\n", lwip_strerr(err));
			tcp_abort(pcb);
		}
		return ERR_OK;
	}
	else
	{
		if (cs->send_buf.left == 0)
			return netd_queue_send(cs, pcb);
		else
		{
			++cs->send_buf.retries;
			/*
			  if (ts->retries == 4) {
			  tcp_abort(pcb);
			  return ERR_ABRT;
			  }
			*/
			send_data(pcb, cs);
			return ERR_OK;
		}
	}
}

static err_t
netd_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
	struct client_state *cs;

	cs = arg;

	cs->send_buf.retries = 0;

	if (cs->send_buf.left > 0)
	{
		send_data(pcb, cs);
		return ERR_OK;
	}
	else
	{
		return netd_queue_send(cs, pcb);
		//printf("\nCLOSED.\n");
		//close_conn(pcb, ts);
	}
}

static err_t
netd_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	struct client_state *cs;
	uint8_t *data;
	size_t q_len_remaining;
	int r;
	
	cs = arg;

	if (err == ERR_OK && p != NULL)
	{
		struct pbuf *q;
		for (q = p; q; q = q->next)
		{
			data = q->payload;
			q_len_remaining = q->len;
			while (q_len_remaining > 0)
			{
				// TODO: can we only write however much is allowed by the pipe
				// space and tcp_recved() this amount?
				// To see how often this is a problem, do a stat and report
				// when write() will likely block:
				if (!quiet)
				{
					struct Stat stat;
					if ((r = fstat(cs->to_client, &stat)) < 0)
						panic("fstat: %e", r);
					const size_t pipe_size = 16*PGSIZE - 2*sizeof(off_t);
					if (pipe_size - stat.st_size < q_len_remaining)
						printf("netd net contention: cs->to_client pipe full\n");
				}

				if ((r = write(cs->to_client, data, q_len_remaining)) < 0)
					panic("write: %e", r);
				data += r;
				q_len_remaining -= r;
			}
		}
		
		tcp_recved(pcb, p->tot_len);
		(void) pbuf_free(p);
	}
	else if (err == ERR_OK && p == NULL)
		close_conn(pcb, cs, ERR_OK);
	
	return ERR_OK;
}

static err_t
netd_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
	struct listen_state* ls;
	struct client_state* cs;

	ls = (struct listen_state*) arg;

	if (!ls->acceptor || envs[ENVX(ls->acceptor)].env_status == ENV_FREE)
	{
		// No env is waiting to accept a new connection, tell lwip not
		// enough memory, which is sort of similar.
		//
		// One would think we could tcp_accept(ls->pcb, NULL) in this
		// function when we set ls->acceptor = 0. However, this quickly causes
		// lwip to assert:
		// lib/net/core/tcp_in.c:574: pcb->accept != NULL
		// and then crash in a checksum fuction. Returning ERR_MEM is a hack
		// workaround to avoid this problem.
		fprintf(STDERR_FILENO, "netd: ");
		if (!ls->acceptor)
			fprintf(STDERR_FILENO, "!ls->acceptor");
		else
			fprintf(STDERR_FILENO, "ls->acceptor no longer around");
		fprintf(STDERR_FILENO, ", on %s:%d, from %s:%d\n",
				  inet_iptoa(pcb->local_ip), pcb->local_port,
				  inet_iptoa(pcb->remote_ip), pcb->remote_port);

		gc_listens();

		return ERR_MEM;
	}

	if (err != ERR_OK)
	{
		conn_err_listen(ls, err);
		return ERR_OK;
	}

	if (debug & DEBUG_CONNSTATUS)
	{
		printf("netd connection accepted %s:%d",
				 inet_iptoa(ls->ipaddr), (int) ls->port);
		printf("<->%s:%d\n",
				 inet_iptoa(pcb->remote_ip), (int) pcb->remote_port);
	}

	tcp_setprio(pcb, TCP_PRIO_MIN);
	
	cs = malloc(sizeof(struct client_state));
	//cs = mem_malloc(sizeof(struct client_state));	
	if (!cs)
	{
		fprintf(STDERR_FILENO, "netd netd_accept: malloc: Out of memory\n");
		conn_err_listen(ls, ERR_MEM);
		return ERR_MEM;
	}
	client_state_init(cs);
	cs->envid       = ls->acceptor;

	ls->acceptor    = 0;
	// Do not:
	// tcp_accept(ls->pcb, NULL);
	// See the the above if(!ls->acceptor) comment for why.

	// Inform client of connect success
	ipc_send(cs->envid, 0, NULL, 0);

	// Setup the client<->netd pipes
	setup_client_netd_pipes(cs->envid, &cs->to_client, &cs->from_client);

	// Send the remote ipaddr and port
	ipc_send(cs->envid, pcb->remote_ip.addr, NULL, 0);
	ipc_send(cs->envid, pcb->remote_port, NULL, 0);

	// Setup tcp functions
	tcp_arg(pcb, cs);
	tcp_err(pcb, conn_err_client);
	tcp_recv(pcb, netd_recv);
	tcp_sent(pcb, netd_sent);
	tcp_poll(pcb, netd_poll, 1);

	// HACK: Give ls->acceptor time to call accept() again to help
	// increase the chance that the netclient calls accept() before
	// lwip calls this function again. The chance of the netclient
	// being able to do this in time decreases as system load increases.
	int i;
	for (i=0; i<20; i++)
		sys_yield();

	return ERR_OK;
}


static err_t
netd_connect(void *arg, struct tcp_pcb *pcb, err_t err)
{
	struct client_state *cs;

	cs = (struct client_state*) arg;

	if (err != ERR_OK)
	{
		conn_err_client(cs, err);
		return ERR_OK;
	}

	if (debug & DEBUG_CONNSTATUS)
		printf("netd connection connected to %s:%d\n",
				 inet_iptoa(pcb->remote_ip),
				 pcb->remote_port);

	// Inform client of connect success
	ipc_send(cs->envid, 0, NULL, 0);

	// Setup networking
	tcp_setprio(pcb, TCP_PRIO_MIN);

	// Setup the client<->netd pipes
	setup_client_netd_pipes(cs->envid, &cs->to_client, &cs->from_client);

	// Setup tcp functions
	tcp_arg(pcb, cs);
	tcp_err(pcb, conn_err_client);
	tcp_recv(pcb, netd_recv);
	tcp_sent(pcb, netd_sent);
	tcp_poll(pcb, netd_poll, 1);

	return ERR_OK;
}
/*---------------------------------------------------------------------------*/
static void
serve_connect(envid_t whom, struct Netreq_connect *req)
{
	struct tcp_pcb *pcb;
	struct client_state *cs;
	err_t err;

	if (debug & DEBUG_REQ)
		printf("netd net request: Connect to %s:%d\n",
				 inet_iptoa(req->req_ipaddr), req->req_port);

	cs = malloc(sizeof(struct client_state));
	if (!cs)
	{
		fprintf(STDERR_FILENO, "netd serve_connect: malloc: Out of memory\n");
		ipc_send(whom, lwip_to_netclient_err(ERR_MEM), NULL, 0);
		return;
	}
	client_state_init(cs);
	cs->envid       = whom;

	pcb = tcp_new();
	if (!pcb)
	{
		fprintf(STDERR_FILENO, "netd serve_connect: tcp_new: Out of memory\n");
		conn_err_client(cs, ERR_MEM);
		return;
	}
	tcp_arg(pcb, cs);
	tcp_err(pcb, conn_err_client);
	err = tcp_connect(pcb, &req->req_ipaddr, req->req_port, netd_connect);
	if (err != ERR_OK)
		panic("tcp_connect: %s", lwip_strerr(err));
}

void
serve_bind_listen(envid_t whom, struct Netreq_bind_listen *req)
{
	struct listen_state* ls;
	struct tcp_pcb *bind_pcb;
	err_t err;

	if (debug & DEBUG_REQ)
		printf("netd net request: Listen on %s:%d\n",
				 inet_iptoa(req->req_ipaddr), req->req_port);

	gc_listens();

	ls = &listen_states[ENVX(whom)];

	if (ls->pcb)
	{
		fprintf(STDERR_FILENO, "netd does not currently support multiple active listens per environment, rejecting request from envid %08x\n", whom);
		ipc_send(whom, lwip_to_netclient_err(ERR_USE), NULL, 0);
		return;
	}

	listen_state_init(ls);


	bind_pcb = tcp_new();
	if (!bind_pcb)
	{
		ipc_send(whom, lwip_to_netclient_err(ERR_MEM), NULL, 0);
		return;
	}

	err = tcp_bind(bind_pcb, &req->req_ipaddr, req->req_port);
	if (err != ERR_OK)
	{
		ipc_send(whom, lwip_to_netclient_err(err), NULL, 0);
		return;
	}

	ls->pcb = tcp_listen(bind_pcb);
	bind_pcb = NULL;
	if (!ls->pcb)
	{
		ipc_send(whom, lwip_to_netclient_err(ERR_MEM), NULL, 0);
		return;
	}
	
	tcp_arg(ls->pcb, ls);
	tcp_err(ls->pcb, conn_err_listen);

	// Record use
	ls->acceptor = 0;
	ls->listener = whom;
	ls->ipaddr   = req->req_ipaddr;
	ls->port     = req->req_port;

	// Notify cient of success
	ipc_send(whom, 0, NULL, 0);
	ipc_send(whom, ENVX(whom), NULL, 0);
}

static void
serve_close_listen(envid_t whom, struct Netreq_close_listen *req)
{
	panic("netd net: TODO");
}

static void
serve_accept(envid_t whom, struct Netreq_accept *req)
{
	struct listen_state* ls;

	if (debug & DEBUG_REQ)
		printf("netd net request: Accept\n");

	if (req->req_listen_key > NENV)
	{
		ipc_send(whom, lwip_to_netclient_err(ERR_CONN), NULL, 0);
		return;
	}
	ls = &listen_states[req->req_listen_key];

	if (!ls->pcb)
	{
		ipc_send(whom, lwip_to_netclient_err(ERR_CONN), NULL, 0);
		return;
	}

	if (ls->acceptor)
	{
		fprintf(STDERR_FILENO,
				  "netd currently only allows one active accept per listen key\n");
		ipc_send(whom, lwip_to_netclient_err(ERR_USE), NULL, 0);
		return;
	}

	ls->acceptor = whom;
	tcp_accept(ls->pcb, netd_accept);
}

static void
serve_stats(envid_t whom, struct Netreq_stats *req)
{
	int r;

	if (debug & DEBUG_REQ)
		printf("netd net request: Stats\n");

	if ((r = fork()) < 0)
	{
		fprintf(STDERR_FILENO, "fork: %e\n", r);
		exit();
	}
	if (r == 0)
	{
		int p[2];

		if ((r = pipe(p)) < 0)
		{
			fprintf(STDERR_FILENO, "pipe: %e\n", r);
			exit();
		}

		if ((r = dup2env_send(p[0], whom)) < 0)
		{
			fprintf(STDERR_FILENO, "dup2env_send: %e\n", r);
			exit();
		}

		if ((r = dup(p[1], STDOUT_FILENO)) < 0)
		{
			fprintf(STDERR_FILENO, "dup: %e\n", r);
			exit();
		}
		if ((r = dup(STDOUT_FILENO, STDERR_FILENO)) < 0)
		{
			fprintf(STDERR_FILENO, "dup: %e\n", r);
			exit();
		}

		if ((r = close(p[0])) < 0)
		{
			fprintf(STDERR_FILENO, "close: %e\n", r);
			exit();
		}
		if ((r = close(p[1])) < 0)
		{
			fprintf(STDERR_FILENO, "close: %e\n", r);
			exit();
		}

		stats_display();

		exit();
	}
}
/*---------------------------------------------------------------------------*/
static size_t
sizeof_netreq(uint32_t req)
{
	switch (req)
	{
		case NETREQ_CONNECT:
			return sizeof(struct Netreq_connect);
		case NETREQ_BIND_LISTEN:
			return sizeof(struct Netreq_bind_listen);
		case NETREQ_CLOSE_LISTEN:
			return sizeof(struct Netreq_close_listen);
		case NETREQ_ACCEPT:
			return sizeof(struct Netreq_accept);
		case NETREQ_STATS:
			return sizeof(struct Netreq_stats);
		default:
			return 0;
	}
}
/*---------------------------------------------------------------------------*/
struct {
	envid_t envid;
	int     fd;
} netd_net_ipcrecv;

uint8_t req_pg[PGSIZE];

static void
netd_net_ipcrecv_comm(void)
{
	int r;
	struct Stat stat;
	envid_t  whom;
	uint32_t req;

	if ((r = fstat(netd_net_ipcrecv.fd, &stat)) < 0)
	{
		fprintf(STDERR_FILENO, "netd fstat: %e", r);
		exit();
	}

	if (stat.st_size <= 0)
		return;

	if ((r = read(netd_net_ipcrecv.fd, &whom, sizeof(whom))) < 0)
		panic("read: %e");
	if ((r = read(netd_net_ipcrecv.fd, &req, sizeof(req))) < 0)
		panic("read: %e");
	if ((r = read(netd_net_ipcrecv.fd, req_pg, sizeof_netreq(req))) < 0)
		panic("read: %e");

	if (!r && sizeof_netreq(req))
	{
		// The other end of pipe is closed
		fprintf(STDERR_FILENO, "netd net: netd ipcrecv has closed pipe, exiting.\n");
		exit();
	}

	if (debug & DEBUG_REQ)
		printf("netd_net_ipcrecv_comm: read request (req struct len = %d)\n", r);

	switch (req)
	{
		case NETREQ_CONNECT:
			serve_connect(whom, (struct Netreq_connect*) req_pg);
			break;
		case NETREQ_BIND_LISTEN:
			serve_bind_listen(whom, (struct Netreq_bind_listen*) req_pg);
			break;
		case NETREQ_CLOSE_LISTEN:
			serve_close_listen(whom, (struct Netreq_close_listen*) req_pg);
			break;
		case NETREQ_ACCEPT:
			serve_accept(whom, (struct Netreq_accept*) req_pg);
			break;
		case NETREQ_STATS:
			serve_stats(whom, (struct Netreq_stats*) req_pg);
			break;
		default:
			fprintf(STDERR_FILENO, "netd net: Invalid request code %d from %08x\n", whom, req);
			break;
	}
}
/*---------------------------------------------------------------------------*/
static void
netd_net(envid_t ipcrecv, int fd, int argc, const char **argv)
{
	net_init();

	struct netif netif;
	struct netif *nif = setup_interface(argc, argv, &netif);
	if (!nif)
	{
		sys_env_destroy(ipcrecv);
		exit();
	}

	netd_net_ipcrecv.envid = ipcrecv;
	netd_net_ipcrecv.fd    = fd;

	net_loop(nif, netd_net_ipcrecv_comm);

	sys_env_destroy(ipcrecv);
	exit();
}

/*---------------------------------------------------------------------------*/
//
// The netd ipc receive process

// Va at which to receive page mappings containing client reqs.
// This is the same va as serv.c's, why not.
#define REQVA (0x10000000 - PGSIZE)

static void
netd_ipcrecv(envid_t net, int fd, int argc, const char **argv)
{
	envid_t  whom;
	uint32_t req;
	int perm;
	int r;

	if (get_pte((void*) REQVA) & PTE_P)
		panic("netd ipcrecv: REQVA already mapped");

	while (1)
	{
		perm = 0;
		req = ipc_recv(&whom, (void*) REQVA, &perm, 0);
		if (debug & DEBUG_IPCRECV)
			printf("netd ipcrecv: request #%d from %08x [page %08x]\n",
					 req, whom, vpt[VPN(REQVA)]);

		// All requests must contain an argument page
		if (!(perm & PTE_P))
		{
			fprintf(STDERR_FILENO, "netd ipcrecv: Invalid request from %08x: no argument page\n", whom);
			continue; // just leave it hanging...
		}

		// Forward the request to netd net
		struct Stat stat;
		if ((r = fstat(fd, &stat)) < 0)
		{
			fprintf(STDERR_FILENO, "netd ipcrecv fstat: %e", r);
			exit();
		}

		if ((r = write(fd, &whom, sizeof(whom))) < 0)
			panic("write: %e");
		if ((r = write(fd, &req, sizeof(req))) < 0)
			panic("write: %e");
		if ((r = write(fd, (void*) REQVA, sizeof_netreq(req))) < 0)
			panic("write: %e");

		sys_page_unmap(0, (void*) REQVA);

		if (!r && sizeof_netreq(req))
		{
			// The other end of pipe is closed
			fprintf(STDERR_FILENO, "netd ipcrecv: netd net has closed pipe, exiting.\n");
			exit();
		}
	}

	assert(0);
}

/*---------------------------------------------------------------------------*/
//
// Netd startup

static void
netd(int argc, const char **argv)
{
	int r;
	int p[2];
	envid_t net_envid = env->env_id;
	envid_t ipcrecv_envid;
	
	// Create the pipe p, through which netd ipcrecv will send requests
	// to netd net
	if ((r = pipe(p)) < 0)
	{
		fprintf(STDERR_FILENO, "netd pipe: %e\n", r);
		exit();
	}

	// Fork off netd ipcrecv and start netd ipcrecv and netd net
	if ((ipcrecv_envid = r = fork()) < 0)
	{
		fprintf(STDERR_FILENO, "netd fork: %e\n", r);
		exit();
	}
	if (!r)
	{
		char name[ENV_NAME_LENGTH];

		snprintf(name, ENV_NAME_LENGTH, "%s:IPC", env->env_name);
		sys_env_set_name(0, name);

		close(p[0]);
		netd_ipcrecv(net_envid, p[1], argc, argv);
	}
	else
	{
		close(p[1]);
		netd_net(ipcrecv_envid, p[0], argc, argv);
	}

	assert(0);
}
/*---------------------------------------------------------------------------*/
static void
print_usage(char *bin)
{
	printf("%s\n", bin);
	printf("Options:\n");
	printf("  -q: be quiet, do not display startup messages\n");
	printf("  -c: display network connects and disconnects\n");
	printf("  -r: display requests\n");
	print_ip_addr_usage();
}

void
umain(int argc, char **argv)
{
	if (argc == 0)
	{
		binaryname = "netd";
		sys_env_set_name(0, "netd");
	}
	if (argc >= 2 && !strcmp("-h", argv[1]))
	{
		print_usage(argv[0]);
		exit();
	}

	if (get_arg_idx(argc, (const char**) argv, "-c"))
		debug |= DEBUG_CONNSTATUS;
	if (get_arg_idx(argc, (const char**) argv, "-r"))
		debug |= DEBUG_REQ;
	quiet = get_arg_idx(argc, (const char**) argv, "-q");

	if (!quiet)
		printf("Netd\n");

	netd(argc, (const char**) argv);
}
