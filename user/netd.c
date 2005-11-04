//
// netd - Network Daemon

// TODO:
// - Support more than one bind_listen() in an environment
// - Optimize buffer sizes/poll period for speed

#include <inc/lib.h>
#include <inc/net.h>
#include <inc/malloc.h>
#include <lib/hash_set.h>

#include "lwip/debug.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"
#include "arch/simple.h"


#define DEBUG_CONNSTATUS (1<<2)
#define DEBUG_REQ        (1<<3)
#define DEBUG_IPCRECV    (1<<4)
#define DEBUG_DNS        (1<<5)

static bool quiet = 0;
static int  debug = 0;

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
static struct listen_state listen_states[NENV];

// This value should probably be about the size you find you need pipes to
// have, to get good throughput.
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
gc_listens(void)
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
		ipc_send(cs->envid, netclient_err, NULL, 0, NULL);
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
		ipc_send(ls->acceptor, lwip_to_netclient_err(err), NULL, 0, NULL);
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
		// ACK data read from the pipe since when originally written if
		// it allows the receive window to increase
		const size_t space_free = MIN(TCP_WND, pipefree(cs->to_client));
		if (pcb->rcv_wnd < space_free)
			tcp_recved(pcb, space_free - pcb->rcv_wnd);

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
				// This write assumes there is enough space in the pipe
				// if the rcv_wnd was large enough. This is true in
				// general except that the rcv_wnd starts out at TCP_WND.
				// To remove the need for this relative size assumption
				// we would need to check at every write. However,
				// it doesn't seem useful at this time to allow PIPEBUFSIZ
				// < TCP_WND, so we simply statically assert this requirement.
				static_assert(PIPEBUFSIZ >= TCP_WND);
				if ((r = write(cs->to_client, data, q_len_remaining)) < 0)
					panic("write: %e", r);
				data += r;
				q_len_remaining -= r;
			}
		}

		tcp_recved(pcb, MIN(TCP_WND, pipefree(cs->to_client)) - pcb->rcv_wnd);
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
	ipc_send(cs->envid, 0, NULL, 0, NULL);

	// Setup the client<->netd pipes
	setup_client_netd_pipes(cs->envid, &cs->to_client, &cs->from_client);

	// Send the remote ipaddr and port
	ipc_send(cs->envid, pcb->remote_ip.addr, NULL, 0, NULL);
	ipc_send(cs->envid, pcb->remote_port, NULL, 0, NULL);

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
	ipc_send(cs->envid, 0, NULL, 0, NULL);

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
//
// DNS resolver

#define DNS_PORT 53

#define DNS_TIMEOUT_MS 1000

#define DNS_CLASS_IN 0x0001

#define DNS_TYPE_A 0x0001

#define DNS_FLAG_QR (1 << 0xF)
#define DNS_FLAG_TC (1 << 0x9)
#define DNS_FLAG_RD (1 << 0x8)
#define DNS_FLAG_RA (1 << 0x7)
#define DNS_FLAG_RCODE (0xF)
#define DNS_FLAG_RCODE_VAL(x) (x & 0xF)

#define DNS_RCODE_NAME 3

typedef struct dns_header {
	uint16_t id;
	uint16_t flags;
	uint16_t qdcount;
	uint16_t ancount;
	uint16_t nscount;
	uint16_t arcount;
} dns_header_t;

typedef struct dns_query {
	char    *qname;
	uint16_t qtype;
	uint16_t qclass;
} dns_query_t;

typedef struct dns_rr {
	char    *name;
	uint16_t type;
	uint16_t class;
	uint32_t ttl;
	uint16_t rdlength;
	uint8_t *rdata;
} dns_rr_t;

typedef struct dns_msg {
	dns_header_t h;
	dns_query_t *qds;
	dns_rr_t    *ans;
//	dns_authserv_t *nss; // not yet supported
//	dns_addrec_t   *ars; // not yet supported
} dns_msg_t;

// +1 at the end is for the extra preceeding byte in a name
#define DNS_REQ_SIZE(namelen) (sizeof(dns_header_t) + sizeof(dns_query_t) - sizeof(char*) + namelen + 1)

typedef struct dns_state {
	envid_t         envid;
	struct udp_pcb *pcb;
	struct dns_msg *req;
	struct pbuf    *p_out;
	uint16_t        p_out_len;
	int32_t         expires; // when the last sent query expires
	size_t          dnsserver_idx; // index of the dns server used for query
	size_t          round_no;
} dns_state_t;

// global transaction identifer, must be unique for each request
static uint16_t dns_xid = 0xABCD;


static err_t
dns_msg2raw(const dns_msg_t *dm, uint8_t *raw, size_t raw_len)
{
	size_t i, n = 0;

	memset(raw, 0, raw_len);

	// copy header

	*(uint16_t*) &raw[n] = htons(dm->h.id);
	n += sizeof(dm->h.id);

	*(uint16_t*) &raw[n] = htons(dm->h.flags);
	n += sizeof(dm->h.flags);

	*(uint16_t*) &raw[n] = htons(dm->h.qdcount);
	n += sizeof(dm->h.qdcount);

	*(uint16_t*) &raw[n] = htons(dm->h.ancount);
	n += sizeof(dm->h.ancount);

	*(uint16_t*) &raw[n] = htons(dm->h.nscount);
	n += sizeof(dm->h.nscount);

	*(uint16_t*) &raw[n] = htons(dm->h.arcount);
	n += sizeof(dm->h.arcount);


	// copy query
	for (i=0; i < dm->h.qdcount; i++)
	{
		const size_t name_len = strlen(dm->qds->qname) + 1;
		char *x = &raw[n];
		char *y = x;
		raw[n] = '.';
		n += 1;
		strcpy(&raw[n], dm->qds[i].qname);		
		n += name_len;
		// mark end-of-string with '.' to make the below conversion easy:
		raw[n-1] = '.';
		raw[n] = 0; // ensure string is null-termed

		// convert dots to indicate number of following characters in label:
		while ((y = strchr(y+1, '.')))
		{
			*(uint8_t*) x = y - (x+1);
			x = y;
		}
		raw[n-1] = 0; // revert end-of-string back to null character


		*(uint16_t*) &raw[n] = htons(dm->qds[i].qtype);
		n += sizeof(dm->qds[i].qtype);

		*(uint16_t*) &raw[n] = htons(dm->qds[i].qclass);
		n += sizeof(dm->qds[i].qclass);
	}

	// not yet supported:
	assert(!dm->h.ancount);
	assert(!dm->h.nscount);
	assert(!dm->h.arcount);

	assert(raw_len == n);

	return ERR_OK;
}

#define DNS_NAME_PTR_MASK 0xC0

// Returns whether this dns-style label is a ptr or not.
static bool
dnsname_is_ptr(uint8_t x)
{ return x & DNS_NAME_PTR_MASK; }

// Returns the offset of the dns-style label ptr.
static uint16_t
dnsname_ptr(uint16_t x)
{ return ntohs(x & ~DNS_NAME_PTR_MASK); }

// strdup a raw dns-style name,
// updates offset to point to the byte after the raw dnsname.
// Convert label lens to dots and decompress dns-style string compression.
static char *
dnsname_raw2string(const uint8_t *raw, size_t *offset)
{
	char *name;
	char *tmpname;
	uint8_t ulen = 0; // length of uncompressed text
	size_t off = *offset; // copy of original offset
	bool contains_ptr = 0;
	size_t coffset; // compressed ptr value
	uint8_t c;
	int i;

	// determine ulen
	for (i=0; ((c = *(uint8_t*) &raw[off+i]));)
	{
		if (dnsname_is_ptr(c))
		{
			coffset = dnsname_ptr(*(uint16_t*) &raw[off+i]);
			contains_ptr = 1;
			break;
		}

		const uint8_t label_len = c;
		ulen += label_len + 1; // +1 for '.'
		i += label_len + 1; // +1 to move to next label
	}

	if (!contains_ptr)
		ulen++; // reserve room for '\0'
	*offset = off + ulen;

	if (contains_ptr)
	{
		if (i > 0)
			ulen++; // reserve room for '.' (to preceed compressed labels)
		*offset += 2; // skip over compression mark and offset
	}

	// dup the uncompressed portion of the name
	if (!ulen)
		tmpname = NULL;
	else
	{
		tmpname = malloc(ulen);
		assert(tmpname);
		memcpy(tmpname, &raw[off], ulen);
	}

	// convert label_lens to dots
	for (i=0; i < ulen;)
	{
		uint8_t label_len = *(uint8_t*) &tmpname[i];
		tmpname[i] = '.';
		i += label_len + 1;
	}
	if (!contains_ptr)
		tmpname[ulen-1] = 0;

	// append the compressed labels
	if (contains_ptr)
	{
		if (ulen > 0)
			tmpname[ulen-1] = '.';

		char *cname = dnsname_raw2string(raw, &coffset); // compressed labels
		assert(cname);
		const size_t cname_len = strlen(cname) + 1;

		char *tmpcname = malloc(ulen + cname_len); // tmpname + cname
		memcpy(tmpcname, tmpname, ulen);
		memcpy(tmpcname + ulen, cname, cname_len);
		free(cname);
		free(tmpname);
		tmpname = tmpcname;
	}

	name = strdup(tmpname + (ulen ? 1 : 0) ); // +1 to skip over first '.'
	free(tmpname);
	return name;
}

// TODO: do some amount of malformed message checking, especially to prevent buffer overflows
static dns_msg_t *
dns_raw2msg(const uint8_t *raw)
{
	dns_msg_t *dm;
	size_t i, n=0;

	dm = malloc(sizeof(*dm));
	if (!dm)
		return NULL;
	memset(dm, 0, sizeof(*dm));

	// copy header

	dm->h.id = ntohs(*(uint16_t*) &raw[n]);
	n += sizeof(dm->h.id);

	dm->h.flags = ntohs(*(uint16_t*) &raw[n]);
	n += sizeof(dm->h.flags);

	if (dm->h.flags & DNS_FLAG_TC)
	{
		fprintf(STDERR_FILENO, "netd: dns resolver received truncated answer\n");
		free(dm);
		return NULL;
	}

	dm->h.qdcount = ntohs(*(uint16_t*) &raw[n]);
	n += sizeof(dm->h.qdcount);

	dm->h.ancount = ntohs(*(uint16_t*) &raw[n]);
	n += sizeof(dm->h.ancount);

	dm->h.nscount = ntohs(*(uint16_t*) &raw[n]);
	n += sizeof(dm->h.nscount);

	dm->h.arcount = ntohs(*(uint16_t*) &raw[n]);
	n += sizeof(dm->h.arcount);

	// copy questions

	dm->qds = malloc(dm->h.qdcount * sizeof(dns_query_t));
	assert(dm->qds);
	memset(dm->qds, 0, dm->h.qdcount * sizeof(dns_query_t));

	for (i=0; i < dm->h.qdcount; i++)
	{
		dm->qds[i].qname = dnsname_raw2string(raw, &n);
		assert(dm->qds[i].qname);

		dm->qds[i].qtype = ntohs(*(uint16_t*) &raw[n]);
		n += sizeof(dm->qds[i].qtype);

		dm->qds[i].qclass = ntohs(*(uint16_t*) &raw[n]);
		n += sizeof(dm->qds[i].qclass);
		if (debug & DEBUG_DNS)
			printf("question for %s, type %d, class %d\n",
				   dm->qds[i].qname, dm->qds[i].qtype, dm->qds[i].qclass);
	}

	// copy rrs

	dm->ans = malloc(dm->h.ancount * sizeof(dns_rr_t));
	memset(dm->ans, 0, dm->h.ancount * sizeof(dns_rr_t));

	for (i=0; i < dm->h.ancount; i++)
	{
		dm->ans[i].name = dnsname_raw2string(raw, &n);
		assert(dm->ans[i].name);

		dm->ans[i].type = ntohs(*(uint16_t*) &raw[n]);
		n += sizeof(dm->ans[i].type);

		dm->ans[i].class = ntohs(*(uint16_t*) &raw[n]);
		n += sizeof(dm->ans[i].class);

		dm->ans[i].ttl = ntohl(*(uint32_t*) &raw[n]);
		n += sizeof(dm->ans[i].ttl);

		dm->ans[i].rdlength = ntohs(*(uint16_t*) &raw[n]);
		n += sizeof(dm->ans[i].rdlength);

		// TODO: should we call dnsname_raw2string() if this RR is a CNAME?
		dm->ans[i].rdata = memdup(&raw[n], dm->ans[i].rdlength);
		assert(dm->ans[i].rdata);
		n += dm->ans[i].rdlength;

		if (debug & DEBUG_DNS)
			printf("RR for %s, class %d, ttl %u, ",
				   dm->ans[i].name, dm->ans[i].class, dm->ans[i].ttl);
		if (dm->ans[i].type == 0x1)
		{
			struct ip_addr ip = *(struct ip_addr*) dm->ans[i].rdata;
			if (debug & DEBUG_DNS)
				printf("A: %s", inet_iptoa(ip));
		}
		else if (dm->ans[i].type == 0x5)
		{
			size_t offset = 0;
			char *name = dnsname_raw2string((char*) dm->ans[i].rdata, &offset);
			if (debug & DEBUG_DNS)
				printf("CNAME: %s", name);
			free(name);
		}
		else
		{
			if (debug & DEBUG_DNS)
				printf("?%u, rdlen %u", dm->ans[i].type, dm->ans[i].rdlength);
		}
		if (debug & DEBUG_DNS)
			printf("\n");
	}

	// ignore ns entries for now

	// ignore ar entries for now

	return dm;
}

// Init a dns_msg_t for querying
static err_t
dns_msg_init_query(dns_msg_t *dm, uint32_t xid, const char *name)
{
	memset(dm, 0, sizeof(*dm));

	dm->h.id      = xid;
	dm->h.flags   = DNS_FLAG_RD;
	dm->h.qdcount = 1;
	dm->h.ancount = 0;
	dm->h.nscount = 0;
	dm->h.arcount = 0;

	dm->qds = malloc(1 * sizeof(dns_query_t));
	if (!dm->qds)
		return ERR_MEM;

	dm->qds[0].qname  = strdup(name);
	if (!dm->qds[0].qname)
	{
		free(dm->qds);
		return ERR_MEM;
	}
	dm->qds[0].qtype  = DNS_TYPE_A;
	dm->qds[0].qclass = DNS_CLASS_IN;

	return ERR_OK;	
}

static void
dns_msg_free(dns_msg_t *dm)
{
	int i;

	for (i=0; i < dm->h.qdcount; i++)
		free(dm->qds[i].qname);
	free(dm->qds);

	for (i=0; i < dm->h.ancount; i++)
	{
		free(dm->ans[i].name);
		free(dm->ans[i].rdata);
	}
	free(dm->ans);

	// ignore ns entries for now

	// ignore ar entries for now

	memset(dm, 0, sizeof(*dm));
}

static hash_set_t *pending_dns_queries = NULL;
static int32_t next_dns_tmr = 0;

static err_t
dns_state_init(dns_state_t *ds, envid_t env, const char *name)
{
	const size_t name_len = strlen(name) + 1;
	err_t err;
	int r;

	memset(ds, 0, sizeof(*ds));

	ds->envid = env;

	ds->p_out_len = DNS_REQ_SIZE(name_len);

	ds->req = malloc(sizeof(*ds->req));
	assert(ds->req);
	err = dns_msg_init_query(ds->req, dns_xid, name);
	assert(err == ERR_OK);

	r = hash_set_insert(pending_dns_queries, ds);
	assert(r >= 0);

	return ERR_OK;
}

static void
dns_state_free(dns_state_t *ds)
{
	if (!hash_set_erase(pending_dns_queries, ds))
		fprintf(STDERR_FILENO, "netd %s(): dns_state was not in the pending_dns_queries\n", __FUNCTION__);

	if (ds->pcb)
		udp_remove(ds->pcb);
	dns_msg_free(ds->req);
	pbuf_free(ds->p_out);
	memset(ds, 0, sizeof(*ds));
	free(ds);
}

// Timeout expired dns requests
static void
dns_tmr()
{
	const int32_t dns_tmr_interval = 20;
	const size_t  max_rounds = 2;
	hash_set_it_t it;
	dns_state_t *ds;

	if (next_dns_tmr - env->env_jiffies > 0)
		return;

	hash_set_it_init(&it, pending_dns_queries);

	while ((ds = hash_set_next(&it)))
	{
		if (ds->expires - env->env_jiffies > 0)
			continue;

		if (debug & DEBUG_DNS)
			printf("dns lookup for %s round %d, server %d timed out\n",
				   ds->req->qds[0].qname, ds->round_no, ds->dnsserver_idx);

		ds->dnsserver_idx++;
		if (ds->dnsserver_idx >= vector_size(get_dns_servers()))
		{
			ds->dnsserver_idx = 0;
			ds->round_no++;
		}

		if (ds->round_no < max_rounds)
		{
			udp_disconnect(ds->pcb);
			udp_remove(ds->pcb);
			ds->pcb = NULL;
			pbuf_free(ds->p_out);
			ds->p_out = NULL;
			static void start_dns_query(dns_state_t*);
			start_dns_query(ds);
		}
		else
		{
			udp_disconnect(ds->pcb);
			envid_t envid = ds->envid;
			dns_state_free(ds);
			ipc_send(envid, -E_TIMEOUT, NULL, 0, NULL);
		}
	}

	next_dns_tmr = env->env_jiffies + dns_tmr_interval;
}

static void
gethostbyname_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, struct ip_addr *addr, u16_t port)
{
	dns_state_t *ds = (dns_state_t *) arg;
	struct ip_addr ip;
	int i, r;

	if (debug & DEBUG_DNS)
		printf("dns reply from %s:%d\n", inet_iptoa(*addr), port);

	dns_msg_t *ans = dns_raw2msg(p->payload);
	if (!ans)
	{
		if (debug & DEBUG_DNS)
			fprintf(STDERR_FILENO, "netd: dns_raw2msg() failed\n");
		r = -E_UNSPECIFIED;
		goto exit;
	}

	if (! (ans->h.flags & DNS_FLAG_QR) )
	{
		if (debug & DEBUG_DNS)
			fprintf(STDERR_FILENO, "netd: reply's flags do not have QR set\n");
		r = -E_UNSPECIFIED;
		goto exit;
	}

	const uint8_t rcode = DNS_FLAG_RCODE_VAL(ans->h.flags);
	if (rcode)
	{
		if (rcode == DNS_RCODE_NAME)
			r = -E_NOT_FOUND;
		else
		{
			fprintf(STDERR_FILENO, "netd: dns reply has rcode %d\n", rcode);
			r = -E_UNSPECIFIED;
		}
		goto exit;
	}

	// find an A RR
	r = -E_UNSPECIFIED;
	for (i=0; i < ans->h.ancount; i++)
	{
		if (ans->ans[i].type == DNS_TYPE_A)
		{
			assert(ans->ans[i].rdlength == sizeof(ip));
			ip = *(struct ip_addr*) ans->ans[i].rdata;
			r = 0;
			break;
		}
	}
	if (r < 0 && (debug & DEBUG_DNS))
		fprintf(STDERR_FILENO, "netd: dns reply has no A RR\n");

  exit:
	(void) pbuf_free(p);
	if (ans)
		dns_msg_free(ans);
	envid_t envid = ds->envid;
	dns_state_free(ds);
	udp_disconnect(pcb);

	ipc_send(envid, (uint32_t) r, NULL, 0, NULL);
	if (r >= 0)
		ipc_send(envid, *(uint32_t*) &ip, NULL, 0, NULL);
}

static void
start_dns_query(dns_state_t *ds)
{
	err_t err;
	vector_t *dns_servers;
	struct ip_addr dns_server;

	dns_servers = get_dns_servers();
	if (!dns_servers || !vector_size(dns_servers))
	{
		fprintf(STDERR_FILENO, "netd: no known dns servers\n");
		ipc_send(ds->envid, lwip_to_netclient_err(ERR_ABRT), NULL, 0, NULL);
		return;
	}

	uint32_t ip = (uint32_t) vector_elt(dns_servers, ds->dnsserver_idx); 
	dns_server = *(struct ip_addr*) &ip;
	//inet_atoip("128.143.2.7", &dns_server); // force a UVa dns server, for testing
	//inet_atoip("127.0.0.1", &dns_server); // force time out, for testing

	// Setup ds

	ds->pcb = udp_new();
	assert(ds->pcb);

	ds->p_out = pbuf_alloc(PBUF_TRANSPORT, ds->p_out_len, PBUF_RAM);
	assert(ds->p_out);

	ds->req->h.id = dns_xid++;

	err = dns_msg2raw(ds->req, (uint8_t *) ds->p_out->payload, ds->p_out_len);
	assert(err == ERR_OK);


	// Setup udp and fire

	udp_recv(ds->pcb, gethostbyname_recv, ds);

	err = udp_bind(ds->pcb, IP_ADDR_ANY, 0);
	if (err != ERR_OK)
	{
		ipc_send(ds->envid, lwip_to_netclient_err(err), NULL, 0, NULL);
		return;
	}

	err = udp_connect(ds->pcb, &dns_server, DNS_PORT);
	if (err != ERR_OK)
	{
		ipc_send(ds->envid, lwip_to_netclient_err(err), NULL, 0, NULL);
		return;
	}

	ds->expires = env->env_jiffies + (DNS_TIMEOUT_MS * HZ / 1000);

	err = udp_send(ds->pcb, ds->p_out);
	if (err != ERR_OK)
	{
		ipc_send(ds->envid, lwip_to_netclient_err(err), NULL, 0, NULL);
		return;
	}
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
		ipc_send(whom, lwip_to_netclient_err(ERR_MEM), NULL, 0, NULL);
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

static void
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
		ipc_send(whom, lwip_to_netclient_err(ERR_USE), NULL, 0, NULL);
		return;
	}

	listen_state_init(ls);


	bind_pcb = tcp_new();
	if (!bind_pcb)
	{
		ipc_send(whom, lwip_to_netclient_err(ERR_MEM), NULL, 0, NULL);
		return;
	}

	err = tcp_bind(bind_pcb, &req->req_ipaddr, req->req_port);
	if (err != ERR_OK)
	{
		ipc_send(whom, lwip_to_netclient_err(err), NULL, 0, NULL);
		return;
	}

	ls->pcb = tcp_listen(bind_pcb);
	bind_pcb = NULL;
	if (!ls->pcb)
	{
		ipc_send(whom, lwip_to_netclient_err(ERR_MEM), NULL, 0, NULL);
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
	ipc_send(whom, 0, NULL, 0, NULL);
	ipc_send(whom, ENVX(whom), NULL, 0, NULL);
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
		ipc_send(whom, lwip_to_netclient_err(ERR_CONN), NULL, 0, NULL);
		return;
	}
	ls = &listen_states[req->req_listen_key];

	if (!ls->pcb)
	{
		ipc_send(whom, lwip_to_netclient_err(ERR_CONN), NULL, 0, NULL);
		return;
	}

	if (ls->acceptor)
	{
		fprintf(STDERR_FILENO,
				  "netd currently only allows one active accept per listen key\n");
		ipc_send(whom, lwip_to_netclient_err(ERR_USE), NULL, 0, NULL);
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

		if ((r = dup2(p[1], STDOUT_FILENO)) < 0)
		{
			fprintf(STDERR_FILENO, "dup2: %e\n", r);
			exit();
		}
		if ((r = dup2(STDOUT_FILENO, STDERR_FILENO)) < 0)
		{
			fprintf(STDERR_FILENO, "dup2: %e\n", r);
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

static void
serve_gethostbyname(envid_t whom, struct Netreq_gethostbyname *req)
{
	dns_state_t *ds;
	err_t err;
	
	if (debug & DEBUG_REQ)
		printf("netd net request: Get host by name\n");

	ds = malloc(sizeof(*ds));
	if (!ds)
	{
		ipc_send(whom, lwip_to_netclient_err(ERR_MEM), NULL, 0, NULL);
		return;
	}

	err = dns_state_init(ds, whom, (const char*) req->name);
	if (err != ERR_OK)
	{
		ipc_send(whom, lwip_to_netclient_err(err), NULL, 0, NULL);
		return;
	}

	start_dns_query(ds);
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
		case NETREQ_GETHOSTBYNAME:
			return sizeof(struct Netreq_gethostbyname);
		default:
			return 0;
	}
}
/*---------------------------------------------------------------------------*/
struct {
	envid_t envid;
	int     fd;
} netd_net_ipcrecv;

static uint8_t req_pg[PGSIZE];

static void
netd_net_ipcrecv_comm(void)
{
	int r;
	struct Stat stat;
	envid_t  whom;
	uint32_t req;

	dns_tmr();

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
		case NETREQ_GETHOSTBYNAME:
			serve_gethostbyname(whom, (struct Netreq_gethostbyname*) req_pg);
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
	pending_dns_queries = hash_set_create();
	assert(pending_dns_queries);

	net_loop(nif, netd_net_ipcrecv_comm);

	sys_env_destroy(ipcrecv);
	exit();
}

/*---------------------------------------------------------------------------*/
//
// The netd ipc receive process

// Va at which to receive page mappings containing client reqs.
// This is where kfs/uhfs.h begins using memory, why not?
#define REQVA (0xC0000000 - PGSIZE)

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
		req = ipc_recv(0, &whom, (void*) REQVA, &perm, NULL, 0);
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
	printf("  -d: display dns resolves\n");
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
	if (get_arg_idx(argc, (const char**) argv, "-d"))
		debug |= DEBUG_DNS;
	quiet = get_arg_idx(argc, (const char**) argv, "-q");

	if (!quiet)
		printf("Netd\n");

	netd(argc, (const char**) argv);
}
