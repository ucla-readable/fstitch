/* 8390.c: A general NS8390 ethernet driver core for linux. */
/*
	Written 1992-94 by Donald Becker.
  
	Copyright 1993 United States Government as represented by the
	Director, National Security Agency.

	This software may be used and distributed according to the terms
	of the GNU General Public License, incorporated herein by reference.

	The author may be reached as becker@scyld.com, or C/O
	Scyld Computing Corporation
	410 Severn Ave., Suite 210
	Annapolis MD 21403

  
  This is the chip-specific code for many 8390-based ethernet adaptors.
  This is not a complete driver, it must be combined with board-specific
  code such as ne.c, wd.c, 3c503.c, etc.

  Seeing how at least eight drivers use this code, (not counting the
  PCMCIA ones either) it is easy to break some card by what seems like
  a simple innocent change. Please contact me or Donald if you think
  you have found something that needs changing. -- PG


  Changelog:

  Paul Gortmaker	: remove set_bit lock, other cleanups.
  Paul Gortmaker	: add ei_get_8390_hdr() so we can pass skb's to 
			  ei_block_input() for eth_io_copy_and_sum().
  Paul Gortmaker	: exchange static int ei_pingpong for a #define,
			  also add better Tx error handling.
  Paul Gortmaker	: rewrite Rx overrun handling as per NS specs.
  Alexey Kuznetsov	: use the 8390's six bit hash multicast filter.
  Paul Gortmaker	: tweak ANK's above multicast changes a bit.
  Paul Gortmaker	: update packet statistics for v2.1.x
  Alan Cox		: support arbitary stupid port mappings on the
  			  68K Macintosh. Support >16bit I/O spaces
  Paul Gortmaker	: add kmod support for auto-loading of the 8390
			  module by all drivers that require it.
  Alan Cox		: Spinlocking work, added 'BUG_83C690'
  Paul Gortmaker	: Separate out Tx timeout code from Tx path.
  Paul Gortmaker	: Remove old unused single Tx buffer code.
  Hayato Fujiwara	: Add m32r support.

  Sources:
  The National Semiconductor LAN Databook, and the 3Com 3c503 databook.

  */

#define NS8390_CORE
#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/queue.h>
#include <inc/stdio.h>

#include <kern/kclock.h>
#include <kern/8390.h>
#include <kern/trap.h>
#include <kern/picirq.h>


/* These are the operational function interfaces to board-specific
   routines.
	void reset_8390(struct net_device *dev)
		Resets the board associated with DEV, including a hardware reset of
		the 8390.  This is only called when there is a transmit timeout, and
		it is always followed by 8390_init().
	void block_output(struct net_device *dev, int count, const unsigned char *buf,
					  int start_page)
		Write the COUNT bytes of BUF to the packet buffer at START_PAGE.  The
		"page" value uses the 8390's 256-byte pages.
	void get_8390_hdr(struct net_device *dev, struct e8390_hdr *hdr, int ring_page)
		Read the 4 byte, page aligned 8390 header. *If* there is a
		subsequent read, it will be of the rest of the packet.
	void block_input(struct net_device *dev, int count, struct sk_buff *skb, int ring_offset)
		Read COUNT bytes from the packet buffer into the skb data area. Start 
		reading from RING_OFFSET, the address as the 8390 sees it.  This will always
		follow the read of the 8390 header. 
*/
#define ei_reset_8390 (ei_local->reset_8390)
#define ei_block_output (ei_local->block_output)
#define ei_block_input (ei_local->block_input)
#define ei_get_8390_hdr (ei_local->get_8390_hdr)

#define ETH_ZLEN 60

#define inb_p inb

// the linux kernel outb functions use the arguments in the reverse order.
// the _p versions of the functions are supposed to pause.
#define outb_back_p(data, port) outb(port, data)
#define outb_back(data, port) outb(port, data)

/* use 0 for production, 1 for verification, >2 for debug */
#ifndef ei_debug
int ei_debug = 1;
#endif

struct net_device theNetDev;

#define MAX_8390_DEVS 1

#define MAX_BUFFER_PACKETS 128
#define PACKET_BUFFER_SIZE 8192

static struct {
	uint16_t pkt_free, pkt_ready;
	struct { uint16_t offset, length; } pkt[MAX_BUFFER_PACKETS];
	uint16_t pb_free, pb_ready;
	/* rather than worry about wrapping packets that cross the border, just fudge the size */
	uint8_t packet_buffer[PACKET_BUFFER_SIZE + 1536];
} ns8390_pkb[MAX_8390_DEVS];

#define pkt_free ns8390_pkb[0].pkt_free
#define pkt_ready ns8390_pkb[0].pkt_ready
#define ns8390_pkt ns8390_pkb[0].pkt
#define pb_free ns8390_pkb[0].pb_free
#define pb_ready ns8390_pkb[0].pb_ready
#define packet_buffer ns8390_pkb[0].packet_buffer

#define READY_PACKETS ((pkt_free - pkt_ready + MAX_BUFFER_PACKETS) % MAX_BUFFER_PACKETS)
#define READY_BUFFER ((pb_free - pb_ready + PACKET_BUFFER_SIZE) % PACKET_BUFFER_SIZE)

/* subtract 1 to keep the buffer from filling entirely and looking empty again */
#define FREE_PACKETS ((MAX_BUFFER_PACKETS - pkt_free + pkt_ready - 1) % MAX_BUFFER_PACKETS)
#define FREE_BUFFER ((PACKET_BUFFER_SIZE - pb_free + pb_ready - 1) % PACKET_BUFFER_SIZE)

/* Index to functions. */
static void ei_tx_intr(struct net_device *dev);
static void ei_tx_err(struct net_device *dev);
void ei_tx_timeout(struct net_device *dev);
static void ei_receive(struct net_device *dev);
static void ei_rx_overrun(struct net_device *dev);
static int ei_start_xmit(const char *data, int length, struct net_device *dev);
static void ei_interrupt(int irq);

/* Routines generic to NS8390-based boards. */
static void NS8390_trigger_send(struct net_device *dev, unsigned int length, int start_page);

/*
 *	SMP and the 8390 setup.
 *
 *	The 8390 isn't exactly designed to be multithreaded on RX/TX. There is
 *	a page register that controls bank and packet buffer access. We guard
 *	this with ei_local->page_lock. Nobody should assume or set the page other
 *	than zero when the lock is not held. Lock holders must restore page 0
 *	before unlocking. Even pure readers must take the lock to protect in 
 *	page 0.
 *
 *	To make life difficult the chip can also be very slow. We therefore can't
 *	just use spinlocks. For the longer lockups we disable the irq the device
 *	sits on and hold the lock. We must hold the lock because there is a dual
 *	processor case other than interrupts (get stats/set multicast list in
 *	parallel with each other and transmit).
 *
 *	Note: in theory we can just disable the irq on the card _but_ there is
 *	a latency on SMP irq delivery. So we can easily go "disable irq" "sync irqs"
 *	enter lock, take the queued irq. So we waddle instead of flying.
 *
 *	Finally by special arrangement for the purpose of being generally 
 *	annoying the transmit function is called bh atomic. That places
 *	restrictions on the user context callers as disable_irq won't save
 *	them.
 */
 
int ei_send_packet(struct net_device *dev, const void * data, int length)
{
	int i;
	uint32_t sum = 0;
	
	/* pre-read the buffer to catch any user faults before sending the packet */
	for(i = 0; i < (length + 3) >> 2; i++)
		sum += ((uint32_t *) data)[i];
	
	return ei_start_xmit(data, length, dev);
}

int ei_tx_reset(struct net_device *dev)
{
	ei_tx_timeout(dev);
	return 0;
}

int ei_get_address(struct net_device *dev, uint8_t * buffer)
{
	memcpy(buffer, dev->dev_addr, 6);
	return 0;
}

int ei_query(struct net_device *dev)
{
	return READY_PACKETS;
}

int ei_get_packet(struct net_device *dev, void * buffer, int length)
{
	if(!READY_PACKETS)
		return -E_BUSY;
	
	if(ns8390_pkt[pkt_ready].length < length)
		length = ns8390_pkt[pkt_ready].length;
	
	/* Note that "buffer" may be a userspace buffer, in which case we might fault... */
	if(buffer)
		memcpy(buffer, &packet_buffer[ns8390_pkt[pkt_ready].offset], length);
	
	length = (ns8390_pkt[pkt_ready].length + 3) & ~0x3;
	
	/* free the buffer */
	//pb_ready = (pb_ready + length) % PACKET_BUFFER_SIZE;
	if((pb_ready += length) >= PACKET_BUFFER_SIZE) pb_ready = 0; /* the size fudging allows us to do this */
	length = ns8390_pkt[pkt_ready].length;
	pkt_ready = (pkt_ready + 1) % MAX_BUFFER_PACKETS;
	
	return length;
}


/**
 * ei_open - Open/initialize the board.
 * @dev: network device to initialize
 *
 * This routine goes all-out, setting everything
 * up anew at each open, even though many of these registers should only
 * need to be set once at boot.
 */
int ei_open(struct net_device *dev)
{
	int r;
#warning fix hard-coded IRQ
	dev->base_addr = 0x300;
	dev->irq = 9;
	dev->name = "eth0";

	//unsigned long flags;
	//struct ei_device *ei_local = &dev->ei; //(struct ei_device *) netdev_priv(dev);

	/* The card I/O part of the driver (e.g. 3c503) can hook a Tx timeout
	    wrapper that does e.g. media check & then calls ei_tx_timeout. */
	/*if(dev->tx_timeout == NULL)
		 dev->tx_timeout = ei_tx_timeout;
	if(dev->watchdog_timeo <= 0)
	dev->watchdog_timeo = TX_TIMEOUT;*/
    
	if(request_irq(dev->irq, ei_interrupt))
		return -1;
	irq_setmask_8259A(irq_mask_8259A & ~(1 << dev->irq));

	r = ne_probe1(dev, dev->base_addr);
	if(r < 0)
	{
		irq_setmask_8259A(irq_mask_8259A | (1 << dev->irq));
		request_irq(dev->irq, NULL);
		return r;
	}
	
	NS8390_init(dev, 1);
	return 0;
}

/**
 * ei_tx_timeout - handle transmit time out condition
 * @dev: network device which has apparently fallen asleep
 *
 * Called by kernel when device never acknowledges a transmit has
 * completed (or failed) - i.e. never posted a Tx related interrupt.
 */

void ei_tx_timeout(struct net_device *dev)
{
	long e8390_base = dev->base_addr;
	struct ei_device *ei_local = &dev->ei;
	int txsr, isr;

	ei_local->stat.tx_errors++;

	txsr = inb(e8390_base+EN0_TSR);
	isr = inb(e8390_base+EN0_ISR);

	printf("%s: Tx timed out, %s TSR=%x, ISR=%x.\n",
		dev->name, (txsr & ENTSR_ABT) ? "excess collisions." :
		(isr) ? "lost interrupt?" : "cable problem?", txsr, isr);

	if(!isr && !ei_local->stat.tx_packets) 
	{
		/* The 8390 probably hasn't gotten on the cable yet. */
		ei_local->interface_num ^= 1;   /* Try a different xcvr.  */
	}

	/* Ugly but a reset can be slow, yet must be protected */
	//disable_irq_nosync(dev->irq);
		
	/* Try to restart the card.  Perhaps the user has fixed something. */
	//ei_reset_8390(dev);
	NS8390_init(dev, 1);
		
	//enable_irq(dev->irq);
}
    
/**
 * ei_start_xmit - begin packet transmission
 * @skb: packet to be sent
 * @dev: network device to which packet is sent
 *
 * Sends a packet to an 8390 network device.
 */
int ei_start_xmit(const char *data, int length, struct net_device *dev)
{
	long e8390_base = dev->base_addr;
	struct ei_device *ei_local = &dev->ei; //(struct ei_device *) netdev_priv(dev);
	int send_length, output_page;
	//unsigned long flags;
	char scratch[ETH_ZLEN];

	/* Mask interrupts from the ethercard. */
	outb_back_p(0x00, e8390_base + EN0_IMR);
	
	
	/*
	 *	Slow phase with lock held.
	 */
	 
	//disable_irq_nosync(dev->irq);
	
	send_length = ETH_ZLEN < length ? length : ETH_ZLEN;
    
	/*
	 * We have two Tx slots available for use. Find the first free
	 * slot, and then perform some sanity checks. With two Tx bufs,
	 * you get very close to transmitting back-to-back packets. With
	 * only one Tx buf, the transmitter sits idle while you reload the
	 * card, leaving a substantial gap between each transmitted packet.
	 */

	if(ei_local->tx1 == 0) 
	{
		output_page = ei_local->tx_start_page;
		ei_local->tx1 = send_length;
		if(ei_debug  &&  ei_local->tx2 > 0)
			printf( "%s: idle transmitter tx2=%d, lasttx=%d, txing=%d.\n",
				dev->name, ei_local->tx2, ei_local->lasttx, ei_local->txing);
	}
	else if(ei_local->tx2 == 0) 
	{
		output_page = ei_local->tx_start_page + TX_PAGES/2;
		ei_local->tx2 = send_length;
		if(ei_debug  &&  ei_local->tx1 > 0)
			printf( "%s: idle transmitter, tx1=%d, lasttx=%d, txing=%d.\n",
				dev->name, ei_local->tx1, ei_local->lasttx, ei_local->txing);
	}
	else
	{	
		return -E_BUSY;
		/* We should never get here. 
		if(ei_debug)
			printf( "%s: No Tx buffers free! tx1=%d tx2=%d last=%d\n",
				dev->name, ei_local->tx1, ei_local->tx2, ei_local->lasttx);
		//netif_stop_queue(dev);
		outb_back_p(ENISR_ALL, e8390_base + EN0_IMR);
		//enable_irq(dev->irq);
		ei_local->stat.tx_errors++;
		return 1;*/
	}

	/*
	 * Okay, now upload the packet and trigger a send if the transmitter
	 * isn't already sending. If it is busy, the interrupt handler will
	 * trigger the send later, upon receiving a Tx done interrupt.
	 */
	 
	if(length == send_length)
		ne_block_output(dev, length, data, output_page);
	else
	{
		memset(scratch, 0, ETH_ZLEN);
		memcpy(scratch, data, length);
		ne_block_output(dev, ETH_ZLEN, scratch, output_page);
	}
		
	if(! ei_local->txing) 
	{
		ei_local->txing = 1;
		NS8390_trigger_send(dev, send_length, output_page);
		//dev->trans_start = jiffies;
		if(output_page == ei_local->tx_start_page) 
		{
			ei_local->tx1 = -1;
			ei_local->lasttx = -1;
		}
		else 
		{
			ei_local->tx2 = -1;
			ei_local->lasttx = -2;
		}
	}
	else
		ei_local->txqueue++;

	/*if(ei_local->tx1  &&  ei_local->tx2)
		netif_stop_queue(dev);
	else
		netif_start_queue(dev);*/

	/* Turn 8390 interrupts back on. */
	outb_back_p(ENISR_ALL, e8390_base + EN0_IMR);
	
	//enable_irq(dev->irq);

	ei_local->stat.tx_bytes += send_length;
    
	return 0;
}

/**
 * ei_interrupt - handle the interrupts from an 8390
 * @irq: interrupt number
 * @dev_id: a pointer to the net_device
 * @regs: unused
 *
 * Handle the ether interface interrupts. We pull packets from
 * the 8390 via the card specific functions and fire them at the networking
 * stack. We also handle transmit completions and wake the transmit path if
 * necessary. We also update the counters and do other housekeeping as
 * needed.
 */

static void ei_interrupt(int irq)
{
	struct net_device *dev = &theNetDev; //dev_id;
	long e8390_base;
	int interrupts, nr_serviced = 0;
	struct ei_device *ei_local;

	if(dev == NULL) 
	{
		printf("%s(): irq %d for unknown device.\n", __FUNCTION__, irq);
		return;// 0; //IRQ_NONE;
	}
    
	e8390_base = dev->base_addr;
	ei_local = &dev->ei;

	/* Change to page 0 and read the intr status reg. */
	outb_back_p(E8390_NODMA+E8390_PAGE0, e8390_base + E8390_CMD);
	if(ei_debug > 3)
		printf( "%s: interrupt(isr=%x).\n", dev->name, inb(e8390_base + EN0_ISR));  // inb_p
    
	/* !!Assumption!! -- we stay in page 0.	 Don't break this. */
	while((interrupts = inb(e8390_base + EN0_ISR)) != 0  // inb_p
		   && ++nr_serviced < MAX_SERVICE) 
	{
		/*if(!netif_running(dev)) {
			printf( "%s: interrupt from stopped card\n", dev->name);
			// rmk - acknowledge the interrupts 
			outb_back_p(interrupts, e8390_base + EN0_ISR);
			interrupts = 0;
			break;
		}*/
		if(interrupts & ENISR_OVER) 
			ei_rx_overrun(dev);
		else if(interrupts & (ENISR_RX+ENISR_RX_ERR)) 
		{
			/* Got a good (?) packet. */
			ei_receive(dev);
		}
		/* Push the next to-transmit packet through. */
		if(interrupts & ENISR_TX)
			ei_tx_intr(dev);
		else if(interrupts & ENISR_TX_ERR)
			ei_tx_err(dev);

		if(interrupts & ENISR_COUNTERS) 
		{
			ei_local->stat.rx_frame_errors += inb_p(e8390_base + EN0_COUNTER0);
			ei_local->stat.rx_crc_errors   += inb_p(e8390_base + EN0_COUNTER1);
			ei_local->stat.rx_missed_errors+= inb_p(e8390_base + EN0_COUNTER2);
			outb_back_p(ENISR_COUNTERS, e8390_base + EN0_ISR); /* Ack intr. */
		}
		
		/* Ignore any RDC interrupts that make it back to here. */
		if(interrupts & ENISR_RDC) 
		{
			outb_back_p(ENISR_RDC, e8390_base + EN0_ISR);
		}

		outb_back_p(E8390_NODMA+E8390_PAGE0+E8390_START, e8390_base + E8390_CMD);
	}
    
	if(interrupts && ei_debug) 
	{
		outb_back_p(E8390_NODMA+E8390_PAGE0+E8390_START, e8390_base + E8390_CMD);
		if(nr_serviced >= MAX_SERVICE) 
		{
			/* 0xFF is valid for a card removal */
			if(interrupts!=0xFF)
				printf( "%s: Too much work at interrupt, status %#2.2x\n", dev->name, interrupts);
			outb_back_p(ENISR_ALL, e8390_base + EN0_ISR); /* Ack. most intrs. */
		} else {
			printf( "%s: unknown interrupt %#2x\n", dev->name, interrupts);
			outb_back_p(0xff, e8390_base + EN0_ISR); /* Ack. all intrs. */
		}
	}
	return; // (nr_serviced > 0); //IRQ_RETVAL(nr_serviced > 0);
}

/**
 * ei_tx_err - handle transmitter error
 * @dev: network device which threw the exception
 *
 * A transmitter error has happened. Most likely excess collisions (which
 * is a fairly normal condition). If the error is one where the Tx will
 * have been aborted, we try and send another one right away, instead of
 * letting the failed packet sit and collect dust in the Tx buffer. This
 * is a much better solution as it avoids kernel based Tx timeouts, and
 * an unnecessary card reset.
 *
 * Called with lock held.
 */

static void ei_tx_err(struct net_device *dev)
{
	long e8390_base = dev->base_addr;
	struct ei_device *ei_local = &dev->ei; //(struct ei_device *) netdev_priv(dev);
	unsigned char txsr = inb_p(e8390_base+EN0_TSR);
	unsigned char tx_was_aborted = txsr & (ENTSR_ABT+ENTSR_FU);

#ifdef VERBOSE_ERROR_DUMP
	printf("%s: transmitter error (%x): ", dev->name, txsr);
	if(txsr & ENTSR_ABT)
		printf("excess-collisions ");
	if(txsr & ENTSR_ND)
		printf("non-deferral ");
	if(txsr & ENTSR_CRS)
		printf("lost-carrier ");
	if(txsr & ENTSR_FU)
		printf("FIFO-underrun ");
	if(txsr & ENTSR_CDH)
		printf("lost-heartbeat ");
	printf("\n");
#endif

	outb_back_p(ENISR_TX_ERR, e8390_base + EN0_ISR); /* Ack intr. */

	if(tx_was_aborted)
		ei_tx_intr(dev);
	else 
	{
		ei_local->stat.tx_errors++;
		if(txsr & ENTSR_CRS) ei_local->stat.tx_carrier_errors++;
		if(txsr & ENTSR_CDH) ei_local->stat.tx_heartbeat_errors++;
		if(txsr & ENTSR_OWC) ei_local->stat.tx_window_errors++;
	}
}

/**
 * ei_tx_intr - transmit interrupt handler
 * @dev: network device for which tx intr is handled
 *
 * We have finished a transmit: check for errors and then trigger the next
 * packet to be sent. Called with lock held.
 */

static void ei_tx_intr(struct net_device *dev)
{
	long e8390_base = dev->base_addr;
	struct ei_device *ei_local = &dev->ei; //(struct ei_device *) netdev_priv(dev);
	int status = inb(e8390_base + EN0_TSR);
    
	outb_back_p(ENISR_TX, e8390_base + EN0_ISR); /* Ack intr. */

	/*
	 * There are two Tx buffers, see which one finished, and trigger
	 * the send of another one if it exists.
	 */
	ei_local->txqueue--;

	if(ei_local->tx1 < 0) 
	{
		if(ei_local->lasttx != 1 && ei_local->lasttx != -1)
			printf("%s: bogus last_tx_buffer %d, tx1=%d.\n",
				ei_local->name, ei_local->lasttx, ei_local->tx1);
		ei_local->tx1 = 0;
		if(ei_local->tx2 > 0) 
		{
			ei_local->txing = 1;
			NS8390_trigger_send(dev, ei_local->tx2, ei_local->tx_start_page + 6);
			//dev->trans_start = jiffies;
			ei_local->tx2 = -1,
			ei_local->lasttx = 2;
		}
		else ei_local->lasttx = 20, ei_local->txing = 0;	
	}
	else if(ei_local->tx2 < 0) 
	{
		if(ei_local->lasttx != 2  &&  ei_local->lasttx != -2)
			printf("%s: bogus last_tx_buffer %d, tx2=%d.\n",
				ei_local->name, ei_local->lasttx, ei_local->tx2);
		ei_local->tx2 = 0;
		if(ei_local->tx1 > 0) 
		{
			ei_local->txing = 1;
			NS8390_trigger_send(dev, ei_local->tx1, ei_local->tx_start_page);
			//dev->trans_start = jiffies;
			ei_local->tx1 = -1;
			ei_local->lasttx = 1;
		}
		else
			ei_local->lasttx = 10, ei_local->txing = 0;
	}
	else printf( "s: unexpected TX-done interrupt, lasttx=%d.\n",
				 ei_local->lasttx);

	/* Minimize Tx latency: update the statistics after we restart TXing. */
	if(status & ENTSR_COL)
		ei_local->stat.collisions++;
	if(status & ENTSR_PTX)
		ei_local->stat.tx_packets++;
	else 
	{
		ei_local->stat.tx_errors++;
		if(status & ENTSR_ABT) 
		{
			ei_local->stat.tx_aborted_errors++;
			ei_local->stat.collisions += 16;
		}
		if(status & ENTSR_CRS) 
			ei_local->stat.tx_carrier_errors++;
		if(status & ENTSR_FU) 
			ei_local->stat.tx_fifo_errors++;
		if(status & ENTSR_CDH)
			ei_local->stat.tx_heartbeat_errors++;
		if(status & ENTSR_OWC)
			ei_local->stat.tx_window_errors++;
	}
	//netif_wake_queue(dev);
}

/**
 * ei_receive - receive some packets
 * @dev: network device with which receive will be run
 *
 * We have a good packet(s), get it/them out of the buffers. 
 * Called with lock held.
 */

static void ei_receive(struct net_device *dev)
{
	long e8390_base = dev->base_addr;
	struct ei_device *ei_local = &dev->ei; //(struct ei_device *) netdev_priv(dev);
	unsigned char rxing_page, this_frame, next_frame;
	unsigned short current_offset;
	int rx_pkt_count = 0;
	struct e8390_pkt_hdr rx_frame;
	int num_rx_pages = ei_local->stop_page-ei_local->rx_start_page;
    
	while(++rx_pkt_count < 10)
	{
		int pkt_len, pkt_stat;
		/* Get the rx page (incoming packet pointer). */
		outb_back_p(E8390_NODMA+E8390_PAGE1, e8390_base + E8390_CMD);
		rxing_page = inb_p(e8390_base + EN1_CURPAG);
		outb_back_p(E8390_NODMA+E8390_PAGE0, e8390_base + E8390_CMD);
		
		/* Remove one frame from the ring.  Boundary is always a page behind. */
		this_frame = inb_p(e8390_base + EN0_BOUNDARY) + 1;
		if(this_frame >= ei_local->stop_page)
			this_frame = ei_local->rx_start_page;
		
		/* Someday we'll omit the previous, iff we never get this message.
		   (There is at least one clone claimed to have a problem.)  
		   
		   Keep quiet if it looks like a card removal. One problem here
		   is that some clones crash in roughly the same way.
		 */
		if(ei_debug > 0  &&  this_frame != ei_local->current_page && (this_frame!=0x0 || rxing_page!=0xFF))
			printf( "%s: mismatched read page pointers %x vs %x.\n",
				   dev->name, this_frame, ei_local->current_page);
		
		if(this_frame == rxing_page)	/* Read all the frames? */
			break;				/* Done for now */
		
		current_offset = this_frame << 8;

		ne_get_8390_hdr(dev, &rx_frame, this_frame);
		
		pkt_len = rx_frame.count - sizeof(struct e8390_pkt_hdr);
		pkt_stat = rx_frame.status;
		
		next_frame = this_frame + 1 + ((pkt_len+4)>>8);
		
		/* Check for bogosity warned by 3c503 book: the status byte is never
		   written.  This happened a lot during testing! This code should be
		   cleaned up someday. */
		if(rx_frame.next != next_frame
			&& rx_frame.next != next_frame + 1
			&& rx_frame.next != next_frame - num_rx_pages
			&& rx_frame.next != next_frame + 1 - num_rx_pages) {
			ei_local->current_page = rxing_page;
			outb_back(ei_local->current_page-1, e8390_base+EN0_BOUNDARY);
			ei_local->stat.rx_errors++;
			continue;
		}

		if(pkt_len < 60  ||  pkt_len > 1518) 
		{
			if(ei_debug)
				printf("%s: bogus packet size: %d, status=%x nxpg=%x.\n",
					   dev->name, rx_frame.count, rx_frame.status,
					   rx_frame.next);
			ei_local->stat.rx_errors++;
			ei_local->stat.rx_length_errors++;
		}
		else if((pkt_stat & 0x0F) == ENRSR_RXOK) 
		{
			int pkt_size = (pkt_len + 3) & ~0x3;
			
			if(!FREE_PACKETS || FREE_BUFFER < pkt_size)
			{
				if(ei_debug)
					printf("%s: Dropping packet from queue to make room for incoming packet\n", dev->name);
				ei_get_packet(dev, NULL, 0);
			}
			if(FREE_PACKETS && FREE_BUFFER >= pkt_size)
			{
				int pn = pkt_free;
				pkt_free = (pkt_free + 1) % MAX_BUFFER_PACKETS;
				ns8390_pkt[pn].offset = pb_free;
				//pb_free = (pb_free + pkt_size) % PACKET_BUFFER_SIZE;
				if((pb_free += pkt_size) > PACKET_BUFFER_SIZE) pb_free = 0; /* the size fudging allows us to to this */
				ns8390_pkt[pn].length = pkt_len;
				
				ne_block_input(dev, pkt_len, &packet_buffer[ns8390_pkt[pn].offset], current_offset + sizeof(rx_frame));
				//dev->last_rx = jiffies;
				
				ei_local->stat.rx_packets++;
				ei_local->stat.rx_bytes += pkt_len;
				if(pkt_stat & ENRSR_PHY)
					ei_local->stat.multicast++;
			}
			else
			{
				if(ei_debug)
					printf("%s: Couldn't allocate a packet buffer of size %d\n", dev->name, pkt_len);
				ei_local->stat.rx_dropped++;
				break;
			}
		} 
		else 
		{
			if(ei_debug)
				printf("%s: bogus packet: status=%x nxpg=%x size=%d\n",
					   dev->name, rx_frame.status, rx_frame.next,
					   rx_frame.count);
			ei_local->stat.rx_errors++;
			/* NB: The NIC counts CRC, frame and missed errors. */
			if(pkt_stat & ENRSR_FO)
				ei_local->stat.rx_fifo_errors++;
		}
		next_frame = rx_frame.next;
		
		/* This _should_ never happen: it's here for avoiding bad clones. */
		if(next_frame >= ei_local->stop_page) {
			printf("%s: next frame inconsistency, %x\n", dev->name,
				   next_frame);
			next_frame = ei_local->rx_start_page;
		}
		ei_local->current_page = next_frame;
		outb_back_p(next_frame-1, e8390_base+EN0_BOUNDARY);
	}

	/* We used to also ack ENISR_OVER here, but that would sometimes mask
	   a real overrun, leaving the 8390 in a stopped state with rec'vr off. */
	outb_back_p(ENISR_RX+ENISR_RX_ERR, e8390_base+EN0_ISR);
	return;
}

/**
 * ei_rx_overrun - handle receiver overrun
 * @dev: network device which threw exception
 *
 * We have a receiver overrun: we have to kick the 8390 to get it started
 * again. Problem is that you have to kick it exactly as NS prescribes in
 * the updated datasheets, or "the NIC may act in an unpredictable manner."
 * This includes causing "the NIC to defer indefinitely when it is stopped
 * on a busy network."  Ugh.
 * Called with lock held. Don't call this with the interrupts off or your
 * computer will hate you - it takes 10ms or so. 
 */

static void ei_rx_overrun(struct net_device *dev)
{
	long e8390_base = dev->base_addr;
	unsigned char was_txing, must_resend = 0;
	struct ei_device *ei_local = &dev->ei; //(struct ei_device *) netdev_priv(dev);
    
	/*
	 * Record whether a Tx was in progress and then issue the
	 * stop command.
	 */
	was_txing = inb(e8390_base+E8390_CMD) & E8390_TRANS; // inb_p
	outb_back_p(E8390_NODMA+E8390_PAGE0+E8390_STOP, e8390_base+E8390_CMD);
    
	if(ei_debug > 1)
		printf("%s: Receiver overrun.\n", dev->name);
	ei_local->stat.rx_over_errors++;
    
	/* 
	 * Wait a full Tx time (1.2ms) + some guard time, NS says 1.6ms total.
	 * Early datasheets said to poll the reset bit, but now they say that
	 * it "is not a reliable indicator and subsequently should be ignored."
	 * We wait at least 10ms.
	 */

	//mdelay(10);

	/*
	 * Reset RBCR[01] back to zero as per magic incantation.
	 */
	outb_back_p(0x00, e8390_base+EN0_RCNTLO);
	outb_back_p(0x00, e8390_base+EN0_RCNTHI);

	/*
	 * See if any Tx was interrupted or not. According to NS, this
	 * step is vital, and skipping it will cause no end of havoc.
	 */

	if(was_txing)
	{ 
		unsigned char tx_completed = inb_p(e8390_base+EN0_ISR) & (ENISR_TX+ENISR_TX_ERR);
		if(!tx_completed)
			must_resend = 1;
	}

	/*
	 * Have to enter loopback mode and then restart the NIC before
	 * you are allowed to slurp packets up off the ring.
	 */
	outb_back_p(E8390_TXOFF, e8390_base + EN0_TXCR);
	outb_back_p(E8390_NODMA + E8390_PAGE0 + E8390_START, e8390_base + E8390_CMD);

	/*
	 * Clear the Rx ring of all the debris, and ack the interrupt.
	 */
	ei_receive(dev);
	outb_back_p(ENISR_OVER, e8390_base+EN0_ISR);

	/*
	 * Leave loopback mode, and resend any packet that got stopped.
	 */
	outb_back_p(E8390_TXCONFIG, e8390_base + EN0_TXCR); 
	if(must_resend)
    		outb_back_p(E8390_NODMA + E8390_PAGE0 + E8390_START + E8390_TRANS, e8390_base + E8390_CMD);
}

/*
 *	Collect the stats. This is called unlocked and from several contexts.
 */
 
 struct net_device_stats *get_stats(struct net_device *dev)
{
	long ioaddr = dev->base_addr;
	struct ei_device *ei_local = &dev->ei; //(struct ei_device *) netdev_priv(dev);
	//unsigned long flags;
    
	/* If the card is stopped, just return the present stats. */
	/*if(!netif_running(dev))
	  return &ei_local->stat;*/

	//spin_lock_irqsave(&ei_local->page_lock,flags);
	/* Read the counter registers, assuming we are in page 0. */
	ei_local->stat.rx_frame_errors += inb_p(ioaddr + EN0_COUNTER0);
	ei_local->stat.rx_crc_errors   += inb_p(ioaddr + EN0_COUNTER1);
	ei_local->stat.rx_missed_errors+= inb_p(ioaddr + EN0_COUNTER2);
	//spin_unlock_irqrestore(&ei_local->page_lock, flags);
    
	return &ei_local->stat;
}

/* This page of functions should be 8390 generic */
/* Follow National Semi's recommendations for initializing the "NIC". */

/**
 * NS8390_init - initialize 8390 hardware
 * @dev: network device to initialize
 * @startp: boolean.  non-zero value to initiate chip processing
 *
 *	Must be called with lock held.
 */

void NS8390_init(struct net_device *dev, int startp)
{
	long e8390_base = dev->base_addr;
	int i;
	struct ei_device *ei_local = &dev->ei; //(struct ei_device *) netdev_priv(dev);
	int endcfg = ei_local->word16
	    ? (0x48 | ENDCFG_WTS | (ei_local->bigendian ? ENDCFG_BOS : 0))
	    : 0x48;

	//printf("initting something at 0x%x\n", e8390_base);
    
	if(sizeof(struct e8390_pkt_hdr)!=4)
    		panic("8390.c: header struct mispacked\n");    
	/* Follow National Semi's recommendations for initing the DP83902. */
	outb_back_p(E8390_NODMA+E8390_PAGE0+E8390_STOP, e8390_base+E8390_CMD); /* 0x21 */
	outb_back_p(endcfg, e8390_base + EN0_DCFG);	/* 0x48 or 0x49 */
	/* Clear the remote byte count registers. */
	outb_back_p(0x00,  e8390_base + EN0_RCNTLO);
	outb_back_p(0x00,  e8390_base + EN0_RCNTHI);
	/* Set to monitor and loopback mode -- this is vital!. */
	outb_back_p(E8390_RXOFF, e8390_base + EN0_RXCR); /* 0x20 */
	outb_back_p(E8390_TXOFF, e8390_base + EN0_TXCR); /* 0x02 */
	/* Set the transmit page and receive ring. */
	outb_back_p(ei_local->tx_start_page, e8390_base + EN0_TPSR);
	ei_local->tx1 = ei_local->tx2 = 0;
	outb_back_p(ei_local->rx_start_page, e8390_base + EN0_STARTPG);
	outb_back_p(ei_local->stop_page-1, e8390_base + EN0_BOUNDARY);	/* 3c503 says 0x3f,NS0x26*/
	ei_local->current_page = ei_local->rx_start_page;		/* assert boundary+1 */
	outb_back_p(ei_local->stop_page, e8390_base + EN0_STOPPG);
	/* Clear the pending interrupts and mask. */
	outb_back_p(0xFF, e8390_base + EN0_ISR);
	outb_back_p(0x00,  e8390_base + EN0_IMR);
    
	/* Copy the station address into the DS8390 registers. */
    outb_back_p(E8390_NODMA + E8390_PAGE1 + E8390_STOP, e8390_base+E8390_CMD); // * 0x61 *
	for(i = 0; i < 6; i++) 
	{
		outb_back_p(dev->dev_addr[i], e8390_base + EN1_PHYS_SHIFT(i));
		if(ei_debug > 1 && inb_p(e8390_base + EN1_PHYS_SHIFT(i))!=dev->dev_addr[i])
			printf("Hw. address read/write mismap %d\n",i);
	}

	outb_back_p(ei_local->rx_start_page, e8390_base + EN1_CURPAG);
	outb_back_p(E8390_NODMA+E8390_PAGE0+E8390_STOP, e8390_base+E8390_CMD);

	//netif_start_queue(dev);
	ei_local->tx1 = ei_local->tx2 = 0;
	ei_local->txing = 0;

	if(startp) 
	{
		outb_back_p(0xff,  e8390_base + EN0_ISR);
		outb_back_p(ENISR_ALL,  e8390_base + EN0_IMR);
		outb_back_p(E8390_NODMA+E8390_PAGE0+E8390_START, e8390_base+E8390_CMD);
		outb_back_p(E8390_TXCONFIG, e8390_base + EN0_TXCR); /* xmit on. */
		/* 3c503 TechMan says rxconfig only after the NIC is started. */
		outb_back_p(E8390_RXCONFIG, e8390_base + EN0_RXCR); /* rx on,  */
	}

	//printf("done initing a card!\n");
}

/* Trigger a transmit start, assuming the length is valid. 
   Always called with the page lock held */
   
static void NS8390_trigger_send(struct net_device *dev, unsigned int length,
								int start_page)
{
	long e8390_base = dev->base_addr;
 	//struct ei_device *ei_local __attribute((unused)) = (struct ei_device *) netdev_priv(dev);
   
	outb_back_p(E8390_NODMA+E8390_PAGE0, e8390_base+E8390_CMD);
    
	if(inb_p(e8390_base) & E8390_TRANS) 
	{
		printf( "%s: trigger_send() called with the transmitter busy.\n", dev->name);
		return;
	}
	outb_back_p(length & 0xff, e8390_base + EN0_TCNTLO);
	outb_back_p(length >> 8, e8390_base + EN0_TCNTHI);
	outb_back_p(start_page, e8390_base + EN0_TPSR);
	outb_back_p(E8390_NODMA+E8390_TRANS+E8390_START, e8390_base+E8390_CMD);
}
