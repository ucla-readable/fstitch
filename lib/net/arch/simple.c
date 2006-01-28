#include <lib/netclient.h>

#include "lwip/debug.h"
#include "lwip/stats.h"
#include "lwip/inet.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dhcp.h"
#include "netif/slipif.h"
#include "netif/etharp.h"
#include "arch/simple.h"
#include <inc/config.h>

static const char *default_ip_jn[4] =
	{DEFAULT_IP_JOSNIC_ADDR, DEFAULT_IP_JOSNIC_NETMASK, DEFAULT_IP_JOSNIC_GW, DEFAULT_IP_JOSNIC_DNS};
static const char *default_ip_sl[4] =
	{DEFAULT_IP_SLIP_ADDR,   DEFAULT_IP_SLIP_NETMASK,   DEFAULT_IP_SLIP_GW,   DEFAULT_IP_SLIP_DNS};
static const char **default_ip[2] =
	{ default_ip_jn, default_ip_sl };


static vector_t *dns_servers = NULL;

vector_t *
get_dns_servers()
{
	return dns_servers;
}


void
print_ip_addr_usage()
{
	printf("Additional ip options: [-addr <ip_addr>] [-gw <ip_addr>] [-netmask <ip_addr>] [-dns <ip_addr>\n");
}

void
setup_ip_addrs(int argc, const char **argv, int jn_sl, bool *josnic_dhcp, struct ip_addr *addr, struct ip_addr *netmask, struct ip_addr *gw, struct ip_addr *dns)
{
	const char *addr_str = get_arg_val(argc, argv, "-addr");

	if(!addr_str || (1 != kinet_atoip(addr_str, addr)))
		if(1 != kinet_atoip(default_ip[jn_sl][0], addr))
			panic("bad default ip addr \"%s\"", default_ip[jn_sl][0]);

	const char *netmask_str = get_arg_val(argc, argv, "-netmask");
	if(!netmask_str || (1 != kinet_atoip(netmask_str, netmask)))
		if(1 != kinet_atoip(default_ip[jn_sl][1], netmask))
			panic("bad default ip netmask \"%s\"", default_ip[jn_sl][1]);

	const char *gw_str = get_arg_val(argc, argv, "-gw");
	if(!gw_str || (1 != kinet_atoip(gw_str, gw)))
		if(1 != kinet_atoip(default_ip[jn_sl][2], gw))
			panic("bad default ip gw \"%s\"", default_ip[jn_sl][2]);

	const char *dns_str = get_arg_val(argc, argv, "-dns");
	if(!dns_str || (1 != kinet_atoip(dns_str, dns)))
		if(1 != kinet_atoip(default_ip[jn_sl][3], dns))
			panic("bad default ip dns \"%s\"", default_ip[jn_sl][3]);

	if (addr_str || netmask_str || gw_str)
		*josnic_dhcp = 0;
	else
		*josnic_dhcp = 1;
}

struct netif*
setup_interface(int argc, const char **argv, struct netif *nif_stayaround)
{
	struct ip_addr ipaddr, netmask, gateway, dns;
	struct netif *nif_jn = 0;
	struct netif *nif_sl = 0;
	bool josnic_dhcp;
	const bool quiet = get_arg_idx(argc, argv, "-q");

#if ALLOW_JOSNIC
	setup_ip_addrs(argc, argv, 0, &josnic_dhcp, &ipaddr, &netmask, &gateway, &dns);
	nif_jn = josnicif_setup(nif_stayaround, josnic_dhcp, ipaddr, netmask, gateway, dns, quiet);
#endif

#if ALLOW_SLIP
	if(!nif_jn)
	{
		setup_ip_addrs(argc, argv, 1, &josnic_dhcp, &ipaddr, &netmask, &gateway, &dns);
		nif_sl = slipif_setup(nif_stayaround, ipaddr, netmask, gateway, dns, quiet);
	}
#endif

	if(nif_jn)
		return nif_jn;
	else if(nif_sl)
		return nif_sl;
	else
	{
#if ALLOW_JOSNIC
		if(!nif_jn)
			kdprintf(STDERR_FILENO, "Unable to allocate a josnic interface.\n");
#endif
#if ALLOW_SLIP
		if(!nif_sl)
			kdprintf(STDERR_FILENO, "Unable to allocate a slip interface.\n");
#endif

		return NULL;
	}
}

int   josnicif_check_inpacket(struct netif *netif);
int   josnicif_input(struct netif *netif);
err_t josnicif_init(struct netif *netif);

void
net_init()
{
	// Allocate another page because the ethernet code uses a good bit
	// of stack space
	sys_page_alloc(0, (void *) (USTACKTOP - 2 * PGSIZE), PTE_U | PTE_W | PTE_P);

	stats_init();
	mem_init();
	memp_init();
	pbuf_init();
	netif_init();
	etharp_init();
	ip_init();
	tcp_init();
	udp_init();
}

struct netif*
slipif_setup(struct netif *netif, struct ip_addr ipaddr, struct ip_addr netmask, struct ip_addr gw, struct ip_addr dns, bool quiet)
{
	struct netif *nif;
	nif = netif_add(netif, &ipaddr, &netmask, &gw, NULL, slipif_init, ip_input);
	if(!nif)
		return NULL;

	netif_set_default(nif);
	netif_set_up(nif);

	if (!dns_servers)
	{
		dns_servers = vector_create_size(1);
		if(!dns_servers)
			return NULL; // TODO: free nif?
	}
	vector_elt_set(dns_servers, 0, (void*) *(uint32_t*) &dns);

	if (!quiet)
	{
		kdprintf(STDERR_FILENO, "%c%c%d up for ", netif->name[0], netif->name[1], netif->num);
		kdprintf(STDERR_FILENO, "%s", kinet_iptoa(ipaddr));
		kdprintf(STDERR_FILENO, "<->");
		kdprintf(STDERR_FILENO, "%s", kinet_iptoa(gw));
		kdprintf(STDERR_FILENO, " over serial port 0x%x (default iface)", ((sio_fd_t)(netif->state))->com_addr);
		kdprintf(STDERR_FILENO, "\n");
	}

	return nif;
}

void
josnicif_print_setup(struct netif *netif)
{
	kdprintf(STDERR_FILENO, "%c%c%d up for ", netif->name[0], netif->name[1], netif->num);
	kdprintf(STDERR_FILENO, "%s", kinet_iptoa(netif->ip_addr));
	//kdprintf(STDERR_FILENO, " using nicd %d", ((struct josnicif*)(netif->state))->nicd);
	kdprintf(STDERR_FILENO, "\n");
}


// Used to know whether to print the configured ip address.
bool dhcp_quiet;

void
josnicif_dhcp_completed(struct netif *netif)
{
	int i;

	if (dns_servers)
		vector_clear(dns_servers);
	else
	{
		dns_servers = vector_create();//_size(netif->dhcp->dns_count);
		if(!dns_servers)
			kdprintf(STDERR_FILENO, "%s(): vector_create_size() failed\n",
					__FUNCTION__);
	}

	for (i=0; i < netif->dhcp->dns_count; i++)
		vector_push_back(dns_servers, (void*) *(uint32_t*) &netif->dhcp->offered_dns_addr[i]);

	if (!dhcp_quiet)
	{
		josnicif_print_setup(netif);
		// Only print the first time:
		dhcp_quiet = 1;
	}
}

struct netif*
josnicif_setup(struct netif *netif, bool dhcp, struct ip_addr ipaddr, struct ip_addr netmask, struct ip_addr gw, struct ip_addr dns, bool quiet)
{
	struct netif *nif;

	if (ENABLE_JOSNIC_DHCP && dhcp)
	{
		IP4_ADDR(&ipaddr,  0,0,0,0);
		IP4_ADDR(&netmask, 0,0,0,0);
		IP4_ADDR(&gw,      0,0,0,0);
	}

	nif = netif_add(netif, &ipaddr, &netmask, &gw, NULL, josnicif_init, ip_input);
	if(!nif)
		return NULL;

	if (!dns_servers)
	{
		dns_servers = vector_create_size(1);
		if(!dns_servers)
			return NULL; // TODO: free nif?
	}
	vector_elt_set(dns_servers, 0, (void*) *(uint32_t*) &dns);

	dhcp_quiet = quiet;
	if (ENABLE_JOSNIC_DHCP && dhcp)
	{
		netif_set_default(nif);
		netif_set_up(nif);
		dhcp_start(nif);
	}
	else
	{
		netif_set_default(nif);
		netif_set_up(nif);
	}

	if (!quiet && !dhcp)
		josnicif_print_setup(nif);

	return nif;
}

void
net_loop(struct netif *nif, void (* poll)(void))
{
	int r;
	// Decide which interface to poll
	int jn_sl = -1;
	if(nif->name[0] == 'j' && nif->name[1] == 'n')
		jn_sl = 0;
	else if(nif->name[0] == 's' && nif->name[1] == 'l')
		jn_sl = 1;
	else
	{
		kdprintf(STDERR_FILENO, "Unknown interface name %c%c\n", nif->name[0], nif->name[1]);
		exit(0);
	}

	//
	// Call timers and don't exit

	// LWIP interval macros are measured in milliseconds
	const int32_t fast_interval        = TCP_FAST_INTERVAL * HZ / 1000;
	const int32_t slow_interval        = TCP_SLOW_INTERVAL * HZ / 1000;
	const int32_t etharp_interval      = ARP_TMR_INTERVAL * HZ / 1000;
	const int32_t dhcp_fine_interval   = DHCP_FINE_TIMER_MSECS * HZ / 1000;
	const int32_t dhcp_coarse_interval = DHCP_COARSE_TIMER_SECS * HZ;

	// njs: number of jiffies
	int32_t cur_njs          = env->env_jiffies;
	int32_t next_njs_slow    = cur_njs;
	int32_t next_njs_fast    = cur_njs;
	int32_t next_etharp      = cur_njs;
	int32_t next_dhcp_fine   = cur_njs;
	int32_t next_dhcp_coarse = cur_njs;

	for(;;)
	{
		if(poll)
			poll();

		int read_data = 0;
		if(jn_sl == 0)
		{
			if ((r = josnicif_check_inpacket(nif)) < 0)
				kdprintf(STDERR_FILENO, "josnicif_check_inpacket: %e\n", r);
			else
				read_data += r;
			josnicif_input(nif);
		}
		else if(jn_sl == 1)
		{
			read_data += slipif_loop_iter(nif);
		}
		else
		{
			panic("No handling for other ifaces yet");
		}

		cur_njs = env->env_jiffies;
		if (next_njs_fast - cur_njs <= 0)
		{
			tcp_fasttmr();
			next_njs_fast = cur_njs + fast_interval;
		}
		if (next_njs_slow - cur_njs <= 0)
		{
			tcp_slowtmr();
			next_njs_slow = cur_njs + slow_interval;
		}
		if (jn_sl == 0 && next_etharp - cur_njs <= 0)
		{
			etharp_tmr();
			next_etharp = cur_njs + etharp_interval;
		}
		if (ENABLE_JOSNIC_DHCP && jn_sl == 0)
		{
			if (next_dhcp_coarse - cur_njs <= 0)
			{
				dhcp_coarse_tmr();
				next_dhcp_coarse = cur_njs + dhcp_coarse_interval;
			}
			if (next_dhcp_fine - cur_njs <= 0)
			{
				dhcp_fine_tmr();
				next_dhcp_fine = cur_njs + dhcp_fine_interval;
			}
		}

		if(!read_data)
			sys_yield();
	}
}
