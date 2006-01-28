#ifndef KUDOS_LIB_NETCLIENT_H
#define KUDOS_LIB_NETCLIENT_H

#ifndef KUDOS_KERNEL
#ifndef __LWIP_IP_ADDR_H__

#if defined(KUDOS)
#include "lwip/ip_addr.h"
#elif defined(UNIXUSER)
#include <netinet/in.h>
#define ip_addr sockaddr_in
#define ip_addr_any INADDR_ANY
#else
#error Unknown target
#endif

int   kgethostbyname(const char *name, struct ip_addr *ipaddr);
int   kconnect(struct ip_addr ipaddr, uint16_t port, int *fd);
int   kbind_listen(struct ip_addr ipaddr, uint16_t port, uint32_t* listen_key);
int   kclose_listen(uint32_t listen_key);
int   kaccept(uint32_t listen_key, int *fd, struct ip_addr* remote_ipaddr, uint16_t* remote_port);

int   knet_stats(int fd);


int   kinet_atoip(const char* cp, struct ip_addr *addr);
char* kinet_iptoa(struct ip_addr addr);

#endif /* !__LWIP_IP_ADDR_H__ */
#endif /* !KUDOS_KERNEL */

#endif /* !KUDOS_LIB_NETCLIENT_H */
