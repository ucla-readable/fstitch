#include <inc/lib.h>

#include "lwip/debug.h"
#include "lwip/stats.h"
#include "lwip/inet.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dhcp.h"
#include "netif/slipif.h"
#include "arch/simple.h"
#include <inc/config.h>

const char *default_ip_jn[3] =
	{DEFAULT_IP_JOSNIC_ADDR, DEFAULT_IP_JOSNIC_NETMASK, DEFAULT_IP_JOSNIC_GW};
const char *default_ip_sl[3] =
	{DEFAULT_IP_SLIP_ADDR,   DEFAULT_IP_SLIP_NETMASK,   DEFAULT_IP_SLIP_GW};
const char **default_ip[2] =
	{ default_ip_jn, default_ip_sl };


void
print_ip_addr_usage()
{
	printf("Additional ip options: [-addr <ip_addr>] [-gw <ip_addr>] [-netmask <ip_addr>]\n");
}

void
setup_ip_addrs(int argc, const char **argv, int jn_sl, bool *josnic_dhcp, struct ip_addr *addr, struct ip_addr *netmask, struct ip_addr *gw)
{
	const char *addr_str = get_arg_val(argc, argv, "-addr");

	if(!addr_str || (1 != inet_atoip(addr_str, addr)))
		if(1 != inet_atoip(default_ip[jn_sl][0], addr))
			panic("bad default ip addr \"%s\"", default_ip[jn_sl][0]);

	const char *netmask_str = get_arg_val(argc, argv, "-netmask");
	if(!netmask_str || (1 != inet_atoip(netmask_str, netmask)))
		if(1 != inet_atoip(default_ip[jn_sl][1], netmask))
			panic("bad default ip netmask \"%s\"", default_ip[jn_sl][1]);

	const char *gw_str = get_arg_val(argc, argv, "-gw");
	if(!gw_str || (1 != inet_atoip(gw_str, gw)))
		if(1 != inet_atoip(default_ip[jn_sl][2], gw))
			panic("bad default ip gw \"%s\"", default_ip[jn_sl][2]);

	if (addr_str || netmask_str || gw_str)
		*josnic_dhcp = 0;
	else
		*josnic_dhcp = 1;
}

struct netif*
setup_interface(int argc, const char **argv, struct netif *nif_stayaround)
{
	struct ip_addr ipaddr, netmask, gateway;
	struct netif *nif_jn = 0;
	struct netif *nif_sl = 0;
	bool josnic_dhcp;
	const bool quiet = get_arg_idx(argc, argv, "-q");

	if(ALLOW_JOSNIC)
	{
		setup_ip_addrs(argc, argv, 0, &josnic_dhcp, &ipaddr, &netmask, &gateway);
		nif_jn = josnicif_setup(nif_stayaround, josnic_dhcp, ipaddr, netmask, gateway, quiet);
	}

	if(ALLOW_SLIP && !nif_jn)
	{
		setup_ip_addrs(argc, argv, 1, &josnic_dhcp, &ipaddr, &netmask, &gateway);
		nif_sl = slipif_setup(nif_stayaround, ipaddr, netmask, gateway, quiet);
	}

	if(nif_jn)
		return nif_jn;
	else if(nif_sl)
		return nif_sl;
	else
	{
		if(ALLOW_JOSNIC && !nif_jn)
			fprintf(STDERR_FILENO, "Unable to allocate a josnic interface.\n");
		if(ALLOW_SLIP && !nif_sl)
			fprintf(STDERR_FILENO, "Unable to allocate a slip interface.\n");

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
	ip_init();
	tcp_init();
	udp_init();
}

struct netif*
slipif_setup(struct netif *netif, struct ip_addr ipaddr, struct ip_addr netmask, struct ip_addr gw, bool quiet)
{
	struct netif *nif;
	nif = netif_add(netif, &ipaddr, &netmask, &gw, NULL, slipif_init, ip_input);
	if(!nif)
		return NULL;

	netif_set_default(nif);
	netif_set_up(nif);

	if (!quiet)
	{
		fprintf(STDERR_FILENO, "%c%c%d up for ", netif->name[0], netif->name[1], netif->num);
		fprintf(STDERR_FILENO, "%s", inet_iptoa(ipaddr));
		fprintf(STDERR_FILENO, "<->");
		fprintf(STDERR_FILENO, "%s", inet_iptoa(gw));
		fprintf(STDERR_FILENO, " over serial port 0x%x (default iface)", ((sio_fd_t)(netif->state))->com_addr);
		fprintf(STDERR_FILENO, "\n");
	}

	return nif;
}

void
josnicif_print_setup(struct netif *netif)
{
	fprintf(STDERR_FILENO, "%c%c%d up for ", netif->name[0], netif->name[1], netif->num);
	fprintf(STDERR_FILENO, "%s", inet_iptoa(netif->ip_addr));
	//fprintf(STDERR_FILENO, " using nicd %d", ((struct josnicif*)(netif->state))->nicd);
	fprintf(STDERR_FILENO, "\n");
}


// Used by dhcp.c to know whether to print the configured ip address.
bool dhcp_quiet;

struct netif*
josnicif_setup(struct netif *netif, bool dhcp, struct ip_addr ipaddr, struct ip_addr netmask, struct ip_addr gw, bool quiet)
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
		fprintf(STDERR_FILENO, "Unknown interface name %c%c\n", nif->name[0], nif->name[1]);
		exit();
	}

	//
	// Call timers and don't exit

	const int32_t fast_interval        = TCP_FAST_INTERVAL / 10;
	const int32_t slow_interval        = TCP_SLOW_INTERVAL / 10;
	const int32_t dhcp_fine_interval   = DHCP_FINE_TIMER_MSECS / 10;
	const int32_t dhcp_coarse_interval = DHCP_COARSE_TIMER_SECS * 100;

	// ncs: num of centi (0.01) secs
	int32_t cur_ncs          = env->env_jiffies;
	int32_t next_ncs_slow    = cur_ncs;
	int32_t next_ncs_fast    = cur_ncs;
	int32_t next_dhcp_fine   = cur_ncs;
	int32_t next_dhcp_coarse = cur_ncs;

	for(;;)
	{
		if(poll)
			poll();

		int read_data = 0;
		if(jn_sl == 0)
		{
			if ((r = josnicif_check_inpacket(nif)) < 0)
				fprintf(STDERR_FILENO, "josnicif_check_inpacket: %e\n");
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

		cur_ncs = env->env_jiffies;
		if (next_ncs_fast - cur_ncs <= 0)
		{
			tcp_fasttmr();
			next_ncs_fast = cur_ncs + fast_interval;
		}
		if (next_ncs_slow - cur_ncs <= 0)
		{
			tcp_slowtmr();
			next_ncs_slow = cur_ncs + slow_interval;
		}
		if (ENABLE_JOSNIC_DHCP && jn_sl == 0)
		{
			if (next_dhcp_coarse - cur_ncs <= 0)
			{
				dhcp_coarse_tmr();
				next_dhcp_coarse = cur_ncs + dhcp_coarse_interval;
			}
			if (next_dhcp_fine - cur_ncs <= 0)
			{
				dhcp_fine_tmr();
				next_dhcp_fine = cur_ncs + dhcp_fine_interval;
			}
		}

		if(!read_data)
			sys_yield();
	}
}
