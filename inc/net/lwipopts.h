/*
 * This file modified from contrib 0.7.1 20040319 unix lib's.
 * Original copyright follows.
 */

/*
 * Copyright (c) 2001-2003 Swedish Institute of Computer Science.
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
#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 1

//#define NO_SYS 1
//#define LWIP_CALLBACK_API 0
/*
#define LWIP_DEBUG

#ifdef LWIP_DEBUG
#define DBG_TYPES_ON DBG_ON

#define DHCP_DEBUG  DBG_ON
#define NETIF_DEBUG DBG_ON
#define PBUF_DEBUG  DBG_ON
#define INET_DEBUG  DBG_ON
#define IP_DEBUG    DBG_ON
#define RAW_DEBUG   DBG_ON
#define MEM_DEBUG   DBG_ON
#define SYS_DEBUG   DBG_ON
#define TCP_DEBUG   DBG_ON
#define TCP_DETAILED_DEBUG DBG_ON
#define TCP_INPUT_DEBUG   TCP_DETAILED_DEBUG
#define TCP_FR_DEBUG      TCP_DETAILED_DEBUG
#define TCP_RTO_DEBUG     TCP_DETAILED_DEBUG
#define TCP_REXMIT_DEBUG  TCP_DETAILED_DEBUG
#define TCP_CWND_DEBUG    TCP_DETAILED_DEBUG
#define TCP_WND_DEBUG     TCP_DETAILED_DEBUG
#define TCP_OUTPUT_DEBUG  TCP_DETAILED_DEBUG
#define TCP_RST_DEBUG     TCP_DETAILED_DEBUG
#define TCP_QLEN_DEBUG    TCP_DETAILED_DEBUG
#define UDP_DEBUG         DBG_ON
#define SLIP_DEBUG  DBG_ON
#endif
*/

/* ---------- Memory options ---------- */
/* MEM_ALIGNMENT: should be set to the alignment of the CPU for which
   lwIP is compiled. 4 byte alignment -> define MEM_ALIGNMENT to 4, 2
   byte alignment -> define MEM_ALIGNMENT to 2. */
#define MEM_ALIGNMENT           4

/* MEM_SIZE: the size of the heap memory. If the application will send
a lot of data that needs to be copied, this should be set high. */
#define MEM_SIZE                5000 // See Simons' 12/23/2004 email

/* MEMP_NUM_PBUF: the number of memp struct pbufs. If the application
   sends a lot of data out of ROM (or other static memory), this
   should be set high. */
#define MEMP_NUM_PBUF           64
/* MEMP_NUM_UDP_PCB: the number of UDP protocol control blocks. One
   per active UDP "connection". */
#define MEMP_NUM_UDP_PCB        8
/* MEMP_NUM_TCP_PCB: the number of simulatenously active TCP
   connections. */
#define MEMP_NUM_TCP_PCB        16
/* MEMP_NUM_TCP_PCB_LISTEN: the number of listening TCP
   connections. */
#define MEMP_NUM_TCP_PCB_LISTEN 16
/* MEMP_NUM_TCP_SEG: the number of simultaneously queued TCP
   segments. */
#define MEMP_NUM_TCP_SEG        32
/* MEMP_NUM_SYS_TIMEOUT: the number of simulateously active
   timeouts. */
#define MEMP_NUM_SYS_TIMEOUT    3
/* MEMP_SANITY_CHECK: (Added by Frost because as of 1.1.0 it defaults to 0.) */
#define MEMP_SANITY_CHECK 1

/* The following four are used only with the sequential API and can be
   set to 0 if the application only will use the raw API. */
/* MEMP_NUM_NETBUF: the number of struct netbufs. */
#define MEMP_NUM_NETBUF         2
/* MEMP_NUM_NETCONN: the number of struct netconns. */
#define MEMP_NUM_NETCONN        4
/* MEMP_NUM_APIMSG: the number of struct api_msg, used for
   communication between the TCP/IP stack and the sequential
   programs. */
#define MEMP_NUM_API_MSG        8
/* MEMP_NUM_TCPIPMSG: the number of struct tcpip_msg, which is used
   for sequential API communication and incoming packets. Used in
   src/api/tcpip.c. */
#define MEMP_NUM_TCPIP_MSG      8

/* These two control is reclaimer functions should be compiled
   in. Should always be turned on (1). */
#define MEM_RECLAIM             1
#define MEMP_RECLAIM            1

/* ---------- Pbuf options ---------- */
/* PBUF_POOL_SIZE: the number of buffers in the pbuf pool. */
#define PBUF_POOL_SIZE          16

/* PBUF_POOL_BUFSIZE: the size of each pbuf in the pbuf pool. */
#define PBUF_POOL_BUFSIZE       2000 // See Simons' 12/23/2004 email

/* PBUF_LINK_HLEN: the number of bytes that should be allocated for a
   link level header. */
#define PBUF_LINK_HLEN          16

/* ---------- TCP options ---------- */
#define LWIP_TCP                1
#define TCP_TTL                 255

/* Controls if TCP should queue segments that arrive out of
   order. Define to 0 if your device is low on memory. */
#define TCP_QUEUE_OOSEQ         1

/* NOTE:
 * TCP_MSS, TCP_SND_BUF, TCP_SND_QUEUELEN, and TCP_WND (and others?)
 * are not independent of each other.
 *
 * - TCP_MSS must be < 1/2 (TCP_WND), a larger difference may be good.
 *
 * A few helpful urls:
 * http://lists.gnu.org/archive/html/lwip-users/2003-01/msg02181.html
 * http://lists.gnu.org/archive/html/lwip-users/2003-01/msg01795.html
 * http://lists.gnu.org/archive/html/lwip-users/2004-03/msg00119.html
 */

/* TCP Maximum segment size. */
#define TCP_MSS                 1440 // 1500 - 40 - 20, MTU minus (IP + TCP) + IP header sizes (allows a single IPIP tunnel)

/* TCP sender buffer space (bytes). */
#define TCP_SND_BUF             2880 // Twice TCP_MSS

/* TCP sender buffer space (pbufs). This must be at least = 2 *
   TCP_SND_BUF/TCP_MSS for things to work. */
#define TCP_SND_QUEUELEN        4 * TCP_SND_BUF/TCP_MSS

/* TCP receive window. */
#define TCP_WND                 2920

/* Maximum number of retransmissions of data segments. */
#define TCP_MAXRTX              12

/* Maximum number of retransmissions of SYN segments. */
#define TCP_SYNMAXRTX           4

/* ---------- ARP options ---------- */
#define ARP_TABLE_SIZE 10
#define ARP_QUEUEING 1
/**
 * - If enabled, cache entries are generated for every kind of ARP traffic or
 * broadcast IP traffic. This enhances behaviour for sending to a dynamic set
 * of hosts, for example if acting as a gateway.
 * - If disabled, cache entries are generated only for IP destination addresses
 * in use by lwIP or applications. This enhances performance if sending to a small,
 * reasonably static number of hosts. Typically for embedded devices.
 */
// TODO: deprecated. Do I need to update anything?
//#define ETHARP_ALWAYS_INSERT 1

/* ---------- IP options ---------- */
/* Define IP_FORWARD to 1 if you wish to have the ability to forward
   IP packets across network interfaces. If you are going to run lwIP
   on a device with only one network interface, define this to 0. */
#define IP_FORWARD              0

/* If defined to 1, IP options are allowed (but not parsed). If
   defined to 0, all packets with IP options are dropped. */
#define IP_OPTIONS              1

/* ---------- ICMP options ---------- */
#define ICMP_TTL                255


/* ---------- DHCP options ---------- */
/* Define LWIP_DHCP to 1 if you want DHCP configuration of
   interfaces. DHCP is not implemented in lwIP 0.5.1, however, so
   turning this on does currently not work. */
#define LWIP_DHCP               1

/* 1 if you want to do an ARP check on the offered address
   (recommended). */
#define DHCP_DOES_ARP_CHECK     1

/* ---------- UDP options ---------- */
#define LWIP_UDP                1
#define UDP_TTL                 255


/* ---------- Statistics options ---------- */
//#define STATS

#ifdef STATS
#define LINK_STATS
#define IP_STATS
#define ICMP_STATS
#define UDP_STATS
#define TCP_STATS
#define MEM_STATS
#define MEMP_STATS
#define PBUF_STATS
#define SYS_STATS
#endif /* STATS */

#endif /* __LWIPOPTS_H__ */
