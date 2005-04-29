#ifndef KUDOS_INC_NET_H
#define KUDOS_INC_NET_H

#include "lwip/ip_addr.h"


// Definitions for requests from net clients to netd

#define NETREQ_CONNECT      1
#define NETREQ_BIND_LISTEN  2
#define NETREQ_CLOSE_LISTEN 3
#define NETREQ_ACCEPT       4
#define NETREQ_STATS        5
#define NETREQ_GETHOSTBYNAME 6

struct Netreq_connect {
	struct ip_addr req_ipaddr;
	uint16_t req_port;
};

struct Netreq_bind_listen {
	struct ip_addr req_ipaddr;
	uint16_t req_port;
};

struct Netreq_close_listen {
	uint32_t req_listen_key;
};

struct Netreq_accept {
	uint32_t req_listen_key;
};


struct Netreq_stats {
};

#define DNS_NAME_MAXLEN 256

struct Netreq_gethostbyname {
	char name[DNS_NAME_MAXLEN];
};

#endif /* !KUDOS_INC_NET_H */
