#ifndef KUDOS_INC_NET_SIMPLE_H
#define KUDOS_INC_NET_SIMPLE_H

#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include <inc/vector.h>

// lwIP has an inet_aton() and inet_ntoa() that use struct in_addr,
// but everything else in lwIP uses struct ip_addr. These functions takes
// care of the conversion.
int   inet_atoip(const char* cp, struct ip_addr *addr);
char* inet_iptoa(struct ip_addr addr); /* returns ptr to static buffer; not reentrant! */

#define ALLOW_SLIP 1
#define ALLOW_JOSNIC 1

int get_arg_idx(int argc, const char **argv, const char *arg_name);
const char* get_arg_val(int argc, const char **argv, const char *arg_name);

vector_t * get_dns_servers();

void print_ip_addr_usage();
void setup_ip_addrs(int argc, const char **argv, int jn_sl, bool *josnic_dhcp, struct ip_addr *addr, struct ip_addr *netmask, struct ip_addr *gw, struct ip_addr *dns);
struct netif* setup_interface(int argc, const char **argv, struct netif *nif);


void net_init();

struct netif* slipif_setup(struct netif *netif, struct ip_addr ipaddr, struct ip_addr netmask, struct ip_addr gw, struct ip_addr dns, bool quiet);

struct netif* josnicif_setup(struct netif *netif, bool dhcp, struct ip_addr ipaddr, struct ip_addr netmask, struct ip_addr gw, struct ip_addr dns, bool quiet);

void net_loop(struct netif *nif, void (* poll)(void));


#endif // !KUDOS_INC_NET_SIMPLE_H
