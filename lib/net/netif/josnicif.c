/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved. 
 * 
 * Redistribution and use in source and binary forms, with or without modification, 
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED 
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT 
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, 
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT 
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING 
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY 
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 * 
 * Author: Adam Dunkels <adam@sics.se>
 *
 */

/*
 * This file is a skeleton for developing Ethernet network interface
 * drivers for lwIP. Add code to the low_level functions and do a
 * search-and-replace for the word "josnicif" to replace it with
 * something that better describes your network interface.
 */

#include <inc/lib.h>
#include <inc/josnic.h>

#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/stats.h"

#include "netif/etharp.h"

/* Define those to better describe your network interface. */
#define IFNAME0 'j'
#define IFNAME1 'n'

struct josnicif {
  struct eth_addr *ethaddr;
  /* Add whatever per-interface state that is needed here. */
  int nicd;
};

static const struct eth_addr ethbroadcast = {{0xff,0xff,0xff,0xff,0xff,0xff}};

/* Forward declarations. */
void  josnicif_input(struct netif *netif);
static err_t josnicif_output(struct netif *netif, struct pbuf *p,
             struct ip_addr *ipaddr);

static err_t
low_level_init(struct netif *netif)
{
  struct josnicif *josnicif = netif->state;
  
  /* set MAC hardware address length */
  netif->hwaddr_len = 6;

  josnicif->nicd = sys_net_ioctl(NET_IOCTL_ALLOCATE, -1, NULL, 0);
  if(josnicif->nicd < 0)
  {
	  // no ethercard found, error msg in josnicif->nicd
	  return ERR_IF;
  }
  
  /* set MAC hardware address */
  sys_net_ioctl(NET_IOCTL_GETADDRESS, josnicif->nicd, netif->hwaddr, 0);

  /* maximum transfer unit */
  netif->mtu = 1500;
  
  /* broadcast capability */
  netif->flags = NETIF_FLAG_BROADCAST;
 
  /* Do whatever else is needed to initialize interface. */  

  return ERR_OK;
}

/*
 * low_level_output():
 *
 * Should do the actual transmission of the packet. The packet is
 * contained in the pbuf that is passed to the function. This pbuf
 * might be chained.
 *
 */

static err_t
low_level_output(struct netif *netif, struct pbuf *p)
{
  struct josnicif *josnicif = netif->state;
  struct pbuf *q;
  uint8_t buffer[1536];
  int offset = 0;
  int retries;

#if ETH_PAD_SIZE
  pbuf_header(p, -ETH_PAD_SIZE);			/* drop the padding word */
#endif

  for(q = p; q != NULL; q = q->next) {
    /* Send the data from the pbuf to the interface, one pbuf at a
       time. The size of the data in each pbuf is kept in the ->len
       variable. */
    if(offset + q->len >= sizeof(buffer))
      panic("packet too big!");
    memcpy(&buffer[offset], q->payload, q->len);
    offset += q->len;
    if(q->len == q->tot_len && q->next) {
      kdprintf(STDERR_FILENO, "josnicif: breaking output packet chain\n");
      break;
    }
  }
  for(retries = 0; retries != 8; retries++) {
    if(!sys_net_ioctl(NET_IOCTL_SEND, josnicif->nicd, buffer, offset))
      break;
    sys_yield();
  }
  if(retries == 8)
    sys_net_ioctl(NET_IOCTL_RESET, josnicif->nicd, NULL, 0);

#if ETH_PAD_SIZE
  pbuf_header(p, ETH_PAD_SIZE);			/* reclaim the padding word */
#endif
  
#if LINK_STATS
  lwip_stats.link.xmit++;
#endif /* LINK_STATS */      

  return ERR_OK;
}

/*
 * low_level_input():
 *
 * Should allocate a pbuf and transfer the bytes of the incoming
 * packet from the interface into the pbuf.
 *
 */

static struct pbuf *
low_level_input(struct netif *netif)
{
  struct josnicif *josnicif = netif->state;
  struct pbuf *p, *q;
  uint8_t buffer[1536];
  int len, offset = 0;
  
  /* Obtain the size of the packet and put it into the "len"
     variable. */
  len = sys_net_ioctl(NET_IOCTL_RECEIVE, josnicif->nicd, buffer, sizeof(buffer));
  if(len <= 0)
    return NULL;

#if ETH_PAD_SIZE
  len += ETH_PAD_SIZE;						/* allow room for Ethernet padding */
#endif

  /* We allocate a pbuf chain of pbufs from the pool. */
  p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
  
  if (p != NULL) {

#if ETH_PAD_SIZE
    pbuf_header(p, -ETH_PAD_SIZE);			/* drop the padding word */
    len -= ETH_PAD_SIZE;
#endif

    /* We iterate over the pbuf chain until we have read the entire
     * packet into the pbuf. */
    for(q = p; q != NULL; q = q->next) {
      /* Read enough bytes to fill this pbuf in the chain. The
       * available data in the pbuf is given by the q->len
       * variable. */
      if(q->len > len)
	      q->len = len;
      memcpy(q->payload, &buffer[offset], q->len);
      q->tot_len = len;
      offset += q->len;
      len -= q->len;
    }

#if ETH_PAD_SIZE
    pbuf_header(p, ETH_PAD_SIZE);			/* reclaim the padding word */
#endif

#if LINK_STATS
    lwip_stats.link.recv++;
#endif /* LINK_STATS */      
  } else {
    /* drop the packet... */
#if LINK_STATS
    lwip_stats.link.memerr++;
    lwip_stats.link.drop++;
#endif /* LINK_STATS */      
  }

  return p;  
}

/*
 * josnicif_output():
 *
 * This function is called by the TCP/IP stack when an IP packet
 * should be sent. It calls the function called low_level_output() to
 * do the actual transmission of the packet.
 *
 */

static err_t
josnicif_output(struct netif *netif, struct pbuf *p,
      struct ip_addr *ipaddr)
{
  
 /* resolve hardware address, then send (or queue) packet */
  return etharp_output(netif, ipaddr, p);
 
}

/*
 * josnicif_input():
 *
 * This function should be called when a packet is ready to be read
 * from the interface. It uses the function low_level_input() that
 * should handle the actual reception of bytes from the network
 * interface.
 *
 */

void
josnicif_input(struct netif *netif)
{
  struct josnicif *josnicif;
  struct eth_hdr *ethhdr;
  struct pbuf *p;

  josnicif = netif->state;
  
  /* move received packet into a new pbuf */
  p = low_level_input(netif);
  /* no packet could be read, silently ignore this */
  if (p == NULL) return;
  /* points to packet payload, which starts with an Ethernet header */
  ethhdr = p->payload;

#if LINK_STATS
  lwip_stats.link.recv++;
#endif /* LINK_STATS */

  ethhdr = p->payload;
    
  switch (htons(ethhdr->type)) {
  /* IP packet? */
  case ETHTYPE_IP:
    /* update ARP table */
    etharp_ip_input(netif, p);
    /* skip Ethernet header */
    pbuf_header(p, -(s16_t) sizeof(struct eth_hdr));
    /* pass to network layer */
    netif->input(p, netif);
    break;
      
    case ETHTYPE_ARP:
      /* pass p to ARP module  */
      etharp_arp_input(netif, josnicif->ethaddr, p);
      break;
    default:
      pbuf_free(p);
      p = NULL;
      break;
  }
}

/*
 * josnicif_check_inpacket():
 *
 * RETURNS:
 * Number of packets ready to be received.
 */

int
josnicif_check_inpacket(struct netif *netif)
{
  struct josnicif *josnicif = netif->state;
  return sys_net_ioctl(NET_IOCTL_QUERY, josnicif->nicd, NULL, 0);
}

/* ethernetif.c used arp_timer() and sys_timeout() but we do not
 * support sys_timeout() and so call etharp_tmr from net_loop().
 */
/*
static void
arp_timer(void *arg)
{
  etharp_tmr();
  sys_timeout(ARP_TMR_INTERVAL, arp_timer, NULL);
}
*/

/*
 * josnicif_init():
 *
 * Should be called at the beginning of the program to set up the
 * network interface. It calls the function low_level_init() to do the
 * actual setup of the hardware.
 *
 */

err_t
josnicif_init(struct netif *netif)
{
  err_t err;
  struct josnicif *josnicif;
    
  josnicif = mem_malloc(sizeof(struct josnicif));
  
  if (josnicif == NULL)
  {
  	LWIP_DEBUGF(NETIF_DEBUG, ("josnicif_init: out of memory\n"));
  	return ERR_MEM;
  }
  
  netif->state = josnicif;
  netif->name[0] = IFNAME0;
  netif->name[1] = IFNAME1;
  netif->output = josnicif_output;
  netif->linkoutput = low_level_output;
  
  josnicif->ethaddr = (struct eth_addr *)&(netif->hwaddr[0]);
  
  if((err = low_level_init(netif)) != ERR_OK)
	  return err;

  etharp_init();

  sys_timeout(ARP_TMR_INTERVAL, arp_timer, NULL);

  return ERR_OK;
}
