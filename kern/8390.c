/* kern/8390.c: derived from the Linux driver for the 8390 chipset, originally by Donald Becker et al. */

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
#include <kern/josnic.h>

#define ETH_ZLEN 60

#define inb_p inb

// the linux kernel outb functions use the arguments in the reverse order.
// the _p versions of the functions are supposed to pause.
#define outb_back_p(data, port) outb(port, data)
#define outb_back(data, port) outb(port, data)

/* use 0 for production, 1 for verification, >2 for debug */
int ei_debug = 1;

struct ns8390 ei_dev[MAX_8390_DEVS];
int ei_devs = 0;

/* Index to functions. */
static void ei_tx_intr(struct ns8390 *dev);
static void ei_tx_err(struct ns8390 *dev);
static void ei_tx_timeout(struct ns8390 *dev);
static void ei_receive(int which);
static void ei_rx_overrun(int which);
static int ei_start_xmit(const char *data, int length, struct ns8390 *dev);
static void ei_interrupt(int irq);

/* Routines generic to NS8390-based boards. */
static void NS8390_trigger_send(struct ns8390 *dev, unsigned int length, int start_page);


int ei_send_packet(int which, const void * data, int length)
{
	int i;
	uint32_t sum = 0;
	
	/* pre-read the buffer to catch any user faults before sending the packet */
	for(i = 0; i < (length + 3) >> 2; i++)
		sum += ((uint32_t *) data)[i];
	
	return ei_start_xmit(data, length, &ei_dev[which]);
}

int ei_tx_reset(int which)
{
	ei_tx_timeout(&ei_dev[which]);
	return 0;
}

int ei_get_address(int which, uint8_t * buffer)
{
	memcpy(buffer, ei_dev[which].phys_addr, 6);
	return 0;
}

int ei_set_filter(int which, int flags)
{
	if(which < 0)
		return -E_INVAL;
	if(ei_devs <= which)
		return -E_NO_DEV;
	
	return -1;
}

int ei_open(int which)
{
	if(request_irq(ei_dev[which].irq, ei_interrupt))
		return -1;
	irq_setmask_8259A(irq_mask_8259A & ~(1 << ei_dev[which].irq));
	
	NS8390_init(&ei_dev[which], 1);
	return 0;
}

int ei_close(int which)
{
	NS8390_init(&ei_dev[which], 0);
	return 0;
}

static void ei_tx_timeout(struct ns8390 *dev)
{
	long e8390_base = dev->base_addr;
	struct ei_device *ei_local = &dev->ei;
	int txsr, isr;

	ei_local->stat.tx_errors++;

	txsr = inb(e8390_base+EN0_TSR);
	isr = inb(e8390_base+EN0_ISR);

	printf("eth%d: Tx timed out, %s TSR=%x, ISR=%x.\n", dev->which, (txsr & ENTSR_ABT) ? "excess collisions." : isr ? "lost interrupt?" : "cable problem?", txsr, isr);

	/* Ugly but a reset can be slow, yet must be protected */
	//disable_irq_nosync(dev->irq);
		
	/* Try to restart the card.  Perhaps the user has fixed something. */
	//ei_reset_8390(dev);
	NS8390_init(dev, 1);
		
	//enable_irq(dev->irq);
}
    
int ei_start_xmit(const char *data, int length, struct ns8390 *dev)
{
	long e8390_base = dev->base_addr;
	struct ei_device *ei_local = &dev->ei; //(struct ei_device *) netdev_priv(dev);
	int send_length, output_page;
	//unsigned long flags;
	char scratch[ETH_ZLEN];

	/* Mask interrupts from the ethercard. */
	outb_back_p(0x00, e8390_base + EN0_IMR);
	
	
	/* Slow phase with lock held. */
	 
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
		if(ei_debug && ei_local->tx2 > 0)
			printf("eth%d: idle transmitter tx2=%d, lasttx=%d, txing=%d.\n", dev->which, ei_local->tx2, ei_local->lasttx, ei_local->txing);
	}
	else if(ei_local->tx2 == 0) 
	{
		output_page = ei_local->tx_start_page + TX_PAGES/2;
		ei_local->tx2 = send_length;
		if(ei_debug && ei_local->tx1 > 0)
			printf("eth%d: idle transmitter, tx1=%d, lasttx=%d, txing=%d.\n", dev->which, ei_local->tx1, ei_local->lasttx, ei_local->txing);
	}
	else
	{	
		return -E_BUSY;
		/* We should never get here. 
		if(ei_debug)
			printf("eth%d: No Tx buffers free! tx1=%d tx2=%d last=%d\n", dev->which, ei_local->tx1, ei_local->tx2, ei_local->lasttx);
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

static void ei_interrupt(int irq)
{
	int e8390_base;
	int interrupts, nr_serviced = 0;
	struct ei_device *ei_local;
	int which;
	
	for(which = 0; which != ei_devs; which++)
		if(ei_dev[which].irq == irq)
			break;
	
	if(which == ei_devs)
	{
		printf("%s(): IRQ %d for unknown device\n", __FUNCTION__, irq);
		return;
	}

	e8390_base = ei_dev[which].base_addr;
	ei_local = &ei_dev[which].ei;

	/* Change to page 0 and read the intr status reg. */
	outb_back_p(E8390_NODMA+E8390_PAGE0, e8390_base + E8390_CMD);
	if(ei_debug > 3)
		printf("eth%d: interrupt(isr=%x).\n", ei_dev[which].which, inb(e8390_base + EN0_ISR));  // inb_p
    
	/* !!Assumption!! -- we stay in page 0.	 Don't break this. */
	while((interrupts = inb(e8390_base + EN0_ISR)) != 0 && ++nr_serviced < MAX_SERVICE) 
	{
		/*if(!netif_running(dev)) {
			printf( "%s: interrupt from stopped card\n", dev->name);
			// rmk - acknowledge the interrupts 
			outb_back_p(interrupts, e8390_base + EN0_ISR);
			interrupts = 0;
			break;
		}*/
		if(interrupts & ENISR_OVER) 
			ei_rx_overrun(which);
		else if(interrupts & (ENISR_RX+ENISR_RX_ERR)) 
		{
			/* Got a good (?) packet. */
			ei_receive(which);
		}
		/* Push the next to-transmit packet through. */
		if(interrupts & ENISR_TX)
			ei_tx_intr(&ei_dev[which]);
		else if(interrupts & ENISR_TX_ERR)
			ei_tx_err(&ei_dev[which]);

		if(interrupts & ENISR_COUNTERS) 
		{
			ei_local->stat.rx_frame_errors += inb_p(e8390_base + EN0_COUNTER0);
			ei_local->stat.rx_crc_errors   += inb_p(e8390_base + EN0_COUNTER1);
			ei_local->stat.rx_missed_errors+= inb_p(e8390_base + EN0_COUNTER2);
			outb_back_p(ENISR_COUNTERS, e8390_base + EN0_ISR); /* Ack intr. */
		}
		
		/* Ignore any RDC interrupts that make it back to here. */
		if(interrupts & ENISR_RDC) 
			outb_back_p(ENISR_RDC, e8390_base + EN0_ISR);

		outb_back_p(E8390_NODMA+E8390_PAGE0+E8390_START, e8390_base + E8390_CMD);
	}
    
	if(interrupts && ei_debug) 
	{
		outb_back_p(E8390_NODMA+E8390_PAGE0+E8390_START, e8390_base + E8390_CMD);
		if(nr_serviced >= MAX_SERVICE) 
		{
			/* 0xFF is valid for a card removal */
			if(interrupts != 0xFF)
				printf("eth%d: Too much work at interrupt, status %#2.2x\n", ei_dev[which].which, interrupts);
			outb_back_p(ENISR_ALL, e8390_base + EN0_ISR); /* Ack. most intrs. */
		}
		else
		{
			printf("eth%d: unknown interrupt %#2x\n", ei_dev[which].which, interrupts);
			outb_back_p(0xff, e8390_base + EN0_ISR); /* Ack. all intrs. */
		}
	}
	return;
}

static void ei_tx_err(struct ns8390 *dev)
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

static void ei_tx_intr(struct ns8390 *dev)
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
			printf("%s: bogus last_tx_buffer %d, tx1=%d.\n", ei_local->name, ei_local->lasttx, ei_local->tx1);
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
			printf("%s: bogus last_tx_buffer %d, tx2=%d.\n", ei_local->name, ei_local->lasttx, ei_local->tx2);
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
	else
		printf( "s: unexpected TX-done interrupt, lasttx=%d.\n", ei_local->lasttx);

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

static void ei_receive(int which)
{
	struct ns8390 * dev = &ei_dev[which];
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
		if(ei_debug > 0 && this_frame != ei_local->current_page && (this_frame!=0x0 || rxing_page!=0xFF))
			printf("eth%d: mismatched read page pointers %x vs %x.\n", dev->which, this_frame, ei_local->current_page);
		
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
		if(rx_frame.next != next_frame &&
		   rx_frame.next != next_frame + 1 &&
		   rx_frame.next != next_frame - num_rx_pages &&
		   rx_frame.next != next_frame + 1 - num_rx_pages)
		{
			ei_local->current_page = rxing_page;
			outb_back(ei_local->current_page-1, e8390_base+EN0_BOUNDARY);
			ei_local->stat.rx_errors++;
			continue;
		}

		if(pkt_len < 60 || pkt_len > 1518) 
		{
			if(ei_debug)
				printf("eth%d: bogus packet size: %d, status=%x nxpg=%x.\n", dev->which, rx_frame.count, rx_frame.status, rx_frame.next);
			ei_local->stat.rx_errors++;
			ei_local->stat.rx_length_errors++;
		}
		else if((pkt_stat & 0x0F) == ENRSR_RXOK) 
		{
			void * buffer = josnic_async_push_packet(dev->which, pkt_len);
			if(buffer)
			{
				ne_block_input(dev, pkt_len, buffer, current_offset + sizeof(rx_frame));
				//dev->last_rx = jiffies;
				
				ei_local->stat.rx_packets++;
				ei_local->stat.rx_bytes += pkt_len;
				if(pkt_stat & ENRSR_PHY)
					ei_local->stat.multicast++;
			}
			else
			{
				if(ei_debug)
					printf("eth%d: Couldn't allocate a packet buffer of size %d\n", dev->which, pkt_len);
				ei_local->stat.rx_dropped++;
				break;
			}
		} 
		else 
		{
			if(ei_debug)
				printf("eth%d: bogus packet: status=%x nxpg=%x size=%d\n", dev->which, rx_frame.status, rx_frame.next, rx_frame.count);
			ei_local->stat.rx_errors++;
			/* NB: The NIC counts CRC, frame and missed errors. */
			if(pkt_stat & ENRSR_FO)
				ei_local->stat.rx_fifo_errors++;
		}
		next_frame = rx_frame.next;
		
		/* This _should_ never happen: it's here for avoiding bad clones. */
		if(next_frame >= ei_local->stop_page)
		{
			printf("eth%d: next frame inconsistency, %x\n", dev->which, next_frame);
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

static void ei_rx_overrun(int which)
{
	struct ns8390 * dev = &ei_dev[which];
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
		printf("eth%d: Receiver overrun.\n", dev->which);
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
	ei_receive(which);
	outb_back_p(ENISR_OVER, e8390_base+EN0_ISR);

	/*
	 * Leave loopback mode, and resend any packet that got stopped.
	 */
	outb_back_p(E8390_TXCONFIG, e8390_base + EN0_TXCR); 
	if(must_resend)
    		outb_back_p(E8390_NODMA + E8390_PAGE0 + E8390_START + E8390_TRANS, e8390_base + E8390_CMD);
}

struct ns8390_stats *get_stats(struct ns8390 *dev)
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

void NS8390_init(struct ns8390 *dev, int startp)
{
	long e8390_base = dev->base_addr;
	int i;
	struct ei_device *ei_local = &dev->ei; //(struct ei_device *) netdev_priv(dev);
	int endcfg = ei_local->word16
	    ? (0x48 | ENDCFG_WTS | (ei_local->bigendian ? ENDCFG_BOS : 0))
	    : 0x48;

	//printf("initting something at 0x%x\n", e8390_base);
    
	if(sizeof(struct e8390_pkt_hdr) != 4)
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
		outb_back_p(dev->phys_addr[i], e8390_base + EN1_PHYS_SHIFT(i));
		if(ei_debug > 1 && inb_p(e8390_base + EN1_PHYS_SHIFT(i))!=dev->phys_addr[i])
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

static void NS8390_trigger_send(struct ns8390 *dev, unsigned int length, int start_page)
{
	long e8390_base = dev->base_addr;
 	//struct ei_device *ei_local __attribute((unused)) = (struct ei_device *) netdev_priv(dev);
   
	outb_back_p(E8390_NODMA+E8390_PAGE0, e8390_base+E8390_CMD);
    
	if(inb_p(e8390_base) & E8390_TRANS) 
	{
		printf("eth%d: trigger_send() called with the transmitter busy.\n", dev->which);
		return;
	}
	outb_back_p(length & 0xff, e8390_base + EN0_TCNTLO);
	outb_back_p(length >> 8, e8390_base + EN0_TCNTHI);
	outb_back_p(start_page, e8390_base + EN0_TPSR);
	outb_back_p(E8390_NODMA+E8390_TRANS+E8390_START, e8390_base+E8390_CMD);
}
