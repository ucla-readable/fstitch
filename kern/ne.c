/* kern/ne.c: derived from the Linux driver for NE[12]000 cards, originally by Donald Becker et al. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/8390.h>
#include <kern/kclock.h>
#include <kern/trap.h>

static void delay(void)
{
	inb(0x84);
	inb(0x84);
	inb(0x84);
	inb(0x84);
}

static void outb_back_p(uint8_t data, uint32_t port)
{
	outb(port, data);
	delay();
}

static uint8_t inb_p(uint32_t port)
{
	uint8_t retVal = inb(port);
	delay();
	return retVal;
}

#define outb_back(data, port) outb(port, data)

#define KERN_INFO
#define KERN_DEBUG
#define KERN_WARNING
#define KERN_EMERG
#define le16_to_cpus(num) do { } while(0)

/* Some defines that people can play with if so inclined. */

/* Do we perform extra sanity checks on stuff? */
#define NE_SANITY_CHECK

/* Do we implement the read before write bugfix? */
/* #define NE_RW_BUGFIX */

/* Do we have a non std. amount of memory? (in units of 256 byte pages) */
/* #define PACKETBUF_MEMSIZE	0x40 */

/* A zero-terminated list of I/O addresses to be probed at boot. */
static unsigned int netcard_portlist[] = {
	0x300, 0x280, 0x320, 0x340, 0x360, 0x380, 0
};

/* ---- No user-serviceable parts below ---- */

#define NE_BASE	(dev->base_addr)
#define NE_CMD	 	0x00
#define NE_DATAPORT	0x10	/* NatSemi-defined port window offset. */
#define NE_RESET	0x1f	/* Issue a read to reset, a write to clear. */
#define NE_IO_EXTENT	0x20

#define NE1SM_START_PG	0x20	/* First page of TX buffer */
#define NE1SM_STOP_PG 	0x40	/* Last page +1 of RX ring */
#define NESM_START_PG	0x40	/* First page of TX buffer */
#define NESM_STOP_PG	0x80	/* Last page +1 of RX ring */

int ne_probe(struct net_device *dev);

//static int ne_open(struct net_device *dev);
//static int ne_close(struct net_device *dev);


/*  Probe for various non-shared-memory ethercards.

   NEx000-clone boards have a Station Address PROM (SAPROM) in the packet
   buffer memory space.  NE2000 clones have 0x57,0x57 in bytes 0x0e,0x0f of
   the SAPROM, while other supposed NE2000 clones must be detected by their
   SA prefix.

   Reading the SAPROM from a word-wide card with the 8390 set in byte-wide
   mode results in doubled values, which can be detected and compensated for.

   The probe is also responsible for initializing the card and filling
   in the 'dev' and 'ei_status' structures.

   We use the minimum memory size for some ethercard product lines, iff we can't
   distinguish models.  You can increase the packet buffer size by setting
   PACKETBUF_MEMSIZE.  Reported Cabletron packet buffer locations are:
	E1010   starts at 0x100 and ends at 0x2000.
	E1010-x starts at 0x100 and ends at 0x8000. ("-x" means "more memory")
	E2010	 starts at 0x100 and ends at 0x4000.
	E2010-x starts at 0x100 and ends at 0xffff.  */

int ne_probe(struct net_device *dev)
{
	unsigned int base_addr = dev->base_addr;

	/* First check any supplied i/o locations. User knows best. <cough> */
	if(base_addr > 0x1ff)	/* Check a single specified location. */
		return ne_probe1(dev, base_addr);
	else if(base_addr != 0)	/* Don't probe at all. */
		return -E_NO_DEV;

	/* Last resort. The semi-risky ISA auto-probe. */
	for(base_addr = 0; netcard_portlist[base_addr] != 0; base_addr++)
	{
		int ioaddr = netcard_portlist[base_addr];
		if(ne_probe1(dev, ioaddr) == 0)
			return 0;
	}

	return -E_NO_DEV;
}

int ne_probe1(struct net_device *dev, int ioaddr)
{
	int i;
	unsigned char SA_prom[32];
	int wordlength = 2;
	const char *name = NULL;
	int start_page, stop_page;
	int neX000, ctron, copam, bad_card;
	int reg0, ret = 0;

	//if(!request_region(ioaddr, NE_IO_EXTENT, dev->name))
	//	return -E_BUSY;

	reg0 = inb_p(ioaddr);
	if(reg0 == 0xFF)
	{
		ret = -E_NO_DEV;
		goto err_out;
	}

	/* Do a preliminary verification that we have a 8390. */
	{
		int regd;
		outb_back_p(E8390_NODMA+E8390_PAGE1+E8390_STOP, ioaddr + E8390_CMD);
		regd = inb_p(ioaddr + 0x0d);
		outb_back_p(0xff, ioaddr + 0x0d);
		outb_back_p(E8390_NODMA+E8390_PAGE0, ioaddr + E8390_CMD);
		inb_p(ioaddr + EN0_COUNTER0); /* Clear the counter by reading. */
		if(inb_p(ioaddr + EN0_COUNTER0) != 0)
		{
			outb_back_p(reg0, ioaddr);
			outb_back_p(regd, ioaddr + 0x0d);	/* Restore the old values. */
			ret = -E_NO_DEV;
			goto err_out;
		}
	}

	printf("ne2k: probe at 0x%x:", ioaddr);

	/* A user with a poor card that fails to ack the reset, or that
	   does not have a valid 0x57,0x57 signature can still use this
	   without having to recompile. Specifying an i/o address along
	   with an otherwise unused dev->mem_end value of "0xBAD" will
	   cause the driver to skip these parts of the probe. */

	bad_card = 0; //((dev->base_addr != 0) && (dev->mem_end == 0xbad));

	/* Reset card. Who knows what dain-bramaged state it was left in. */

	{
		unsigned long reset_start_time = jiffies;

		/* DON'T change these to inb_p/outb_back_p or reset will fail on clones. */
		outb_back(inb(ioaddr + NE_RESET), ioaddr + NE_RESET);

		while((inb_p(ioaddr + EN0_ISR) & ENISR_RESET) == 0)
			if(jiffies - reset_start_time > 2)
			{
				if(bad_card)
				{
					printf(" (warning: no reset ack)");
					break;
				}
				else
				{
					printf(" not found (no reset ack).\n");
					ret = -E_NO_DEV;
					goto err_out;
				}
			}

		outb_back_p(0xff, ioaddr + EN0_ISR);		/* Ack all intr. */
	}

	/* Read the 16 bytes of station address PROM.
	   We must first initialize registers, similar to NS8390_init(eifdev, 0).
	   We can't reliably read the SAPROM address without this.
	   (I learned the hard way!). */
	{
		struct {
			unsigned char value, offset;
		} program_seq[] = {
			{E8390_NODMA+E8390_PAGE0+E8390_STOP, E8390_CMD}, /* Select page 0*/
			{0x48,	EN0_DCFG},	/* Set byte-wide (0x48) access. */
			{0x00,	EN0_RCNTLO},	/* Clear the count regs. */
			{0x00,	EN0_RCNTHI},
			{0x00,	EN0_IMR},	/* Mask completion irq. */
			{0xFF,	EN0_ISR},
			{E8390_RXOFF, EN0_RXCR},	/* 0x20  Set to monitor */
			{E8390_TXOFF, EN0_TXCR},	/* 0x02  and loopback mode. */
			{32,	EN0_RCNTLO},
			{0x00,	EN0_RCNTHI},
			{0x00,	EN0_RSARLO},	/* DMA starting at 0x0000. */
			{0x00,	EN0_RSARHI},
			{E8390_RREAD+E8390_START, E8390_CMD},
		};

		for(i = 0; i < sizeof(program_seq)/sizeof(program_seq[0]); i++)
			outb_back_p(program_seq[i].value, ioaddr + program_seq[i].offset);

	}
	for(i = 0; i < 32 /*sizeof(SA_prom)*/; i+=2) {
		SA_prom[i] = inb(ioaddr + NE_DATAPORT);
		SA_prom[i+1] = inb(ioaddr + NE_DATAPORT);
		if(SA_prom[i] != SA_prom[i+1])
			wordlength = 1;
	}

	if(wordlength == 2)
	{
		for(i = 0; i < 16; i++)
			SA_prom[i] = SA_prom[i+i];
		/* We must set the 8390 for word mode. */
		outb_back_p(0x49, ioaddr + EN0_DCFG);
		start_page = NESM_START_PG;
		stop_page = NESM_STOP_PG;
	} else {
		start_page = NE1SM_START_PG;
		stop_page = NE1SM_STOP_PG;
	}

	neX000 = (SA_prom[14] == 0x57  &&  SA_prom[15] == 0x57);
	ctron =  (SA_prom[0] == 0x00 && SA_prom[1] == 0x00 && SA_prom[2] == 0x1d);
	copam =  (SA_prom[14] == 0x49 && SA_prom[15] == 0x00);

	/* Set up the rest of the parameters. */
	if(neX000 || bad_card || copam)
		name = (wordlength == 2) ? "NE2000" : "NE1000";
	else if(ctron)
	{
		name = (wordlength == 2) ? "Ctron-8" : "Ctron-16";
		start_page = 0x01;
		stop_page = (wordlength == 2) ? 0x40 : 0x20;
	}
	else
	{
		printf(" not found.\n");
		ret = -E_NO_DEV;
		goto err_out;
	}

	if(dev->irq < 2)
	{
		//unsigned long cookie = probe_irq_on();
		outb_back_p(0x50, ioaddr + EN0_IMR);	/* Enable one interrupt. */
		outb_back_p(0x00, ioaddr + EN0_RCNTLO);
		outb_back_p(0x00, ioaddr + EN0_RCNTHI);
		outb_back_p(E8390_RREAD+E8390_START, ioaddr); /* Trigger it... */
#warning add IRQ detection here
		panic("add IRQ detection here");
		//mdelay(10);		/* wait 10ms for interrupt to propagate */
		outb_back_p(0x00, ioaddr + EN0_IMR); 		/* Mask it again. */
		//dev->irq = probe_irq_off(cookie);
		if(ei_debug > 2)
			printf(" autoirq is %d\n", dev->irq);
	}
	else if(dev->irq == 2)
		/* Fixup for users that don't know that IRQ 2 is really IRQ 9,
		   or don't know which one to set. */
		dev->irq = 9;

	if(!dev->irq)
	{
		printf(" failed to detect IRQ line.\n");
		ret = -E_NO_DEV;
		goto err_out;
	}

	/* Allocate dev->priv and fill in 8390 specific dev fields. */
	/*if(ethdev_init(dev))
	{
        	printf(" unable to get memory for dev->priv.\n");
        	ret = -E_NO_MEM;
		goto err_out;
	}*/

	/* Snarf the interrupt now.  There's no point in waiting since we cannot
	   share and the board will usually be enabled. */
	//ret = request_irq(dev->irq, ei_interrupt);
	if(ret)
	{
		printf(" unable to get IRQ %d (errno=%d).\n", dev->irq, ret);
		goto err_out_kfree;
	}

	dev->base_addr = ioaddr;

	for(i = 0; i < ETHER_ADDR_LEN; i++)
	{
		printf(" %x", SA_prom[i]);
		dev->dev_addr[i] = SA_prom[i];
	}

	printf("\n%s: %s found at 0x%x, using IRQ %d.\n", dev->name, name, ioaddr, dev->irq);

	ei_status.name = name;
	ei_status.tx_start_page = start_page;
	ei_status.stop_page = stop_page;
	ei_status.word16 = (wordlength == 2);

	ei_status.rx_start_page = start_page + TX_PAGES;
#ifdef PACKETBUF_MEMSIZE
	 /* Allow the packet buffer size to be overridden by know-it-alls. */
	ei_status.stop_page = ei_status.tx_start_page + PACKETBUF_MEMSIZE;
#endif

	/*ei_status.reset_8390 = &ne_reset_8390;
	ei_status.block_input = &ne_block_input;
	ei_status.block_output = &ne_block_output;
	ei_status.get_8390_hdr = &ne_get_8390_hdr;*/
	ei_status.priv = 0;
	//dev->open = &ne_open;
	//dev->stop = &ne_close;
	NS8390_init(dev, 0);
	return 0;

err_out_kfree:
	//kfree(dev->priv);
	//dev->priv = NULL;
err_out:
	//release_region(ioaddr, NE_IO_EXTENT);
	return ret;
}

/*static int ne_open(struct net_device *dev)
{
	ei_open(dev);
	return 0;
}

static int ne_close(struct net_device *dev)
{
	if(ei_debug > 1)
		printf( "%s: Shutting down ethercard.\n", dev->name);
	ei_close(dev);
	return 0;
}*/

/* Hard reset the card.  This used to pause for the same period that a
   8390 reset command required, but that shouldn't be necessary. */

void ne_reset_8390(struct net_device *dev)
{
	unsigned long reset_start_time = jiffies;

	if(ei_debug > 1)
		printf("resetting the 8390 t=%ld...", jiffies);

	/* DON'T change these to inb_p/outb_back_p or reset will fail on clones. */
	outb_back(inb(NE_BASE + NE_RESET), NE_BASE + NE_RESET);

	ei_status.txing = 0;
	ei_status.dmaing = 0;

	/* This check _should_not_ be necessary, omit eventually. */
	while((inb_p(NE_BASE+EN0_ISR) & ENISR_RESET) == 0)
		if(jiffies - reset_start_time > 2)
		{
			printf("%s: ne_reset_8390() did not complete.\n", dev->name);
			break;
		}
	outb_back_p(ENISR_RESET, NE_BASE + EN0_ISR);	/* Ack intr. */
}

/* Grab the 8390 specific header. Similar to the block_input routine, but
   we don't need to be concerned with ring wrap as the header will be at
   the start of a page, so we optimize accordingly. */

void ne_get_8390_hdr(struct net_device *dev, struct e8390_pkt_hdr *hdr, int ring_page)
{
	int nic_base = dev->base_addr;

	/* This *shouldn't* happen. If it does, it's the last thing you'll see */

	if(ei_status.dmaing)
	{
		printf("%s: DMAing conflict in ne_get_8390_hdr [DMAstat:%d][irqlock:%d].\n", dev->name, ei_status.dmaing, ei_status.irqlock);
		return;
	}

	ei_status.dmaing |= 0x01;
	outb_back_p(E8390_NODMA+E8390_PAGE0+E8390_START, nic_base+ NE_CMD);
	outb_back_p(sizeof(struct e8390_pkt_hdr), nic_base + EN0_RCNTLO);
	outb_back_p(0, nic_base + EN0_RCNTHI);
	outb_back_p(0, nic_base + EN0_RSARLO);		/* On page boundary */
	outb_back_p(ring_page, nic_base + EN0_RSARHI);
	outb_back_p(E8390_RREAD+E8390_START, nic_base + NE_CMD);

	if(ei_status.word16)
		insw(NE_BASE + NE_DATAPORT, hdr, sizeof(struct e8390_pkt_hdr)>>1);
	else
		insb(NE_BASE + NE_DATAPORT, hdr, sizeof(struct e8390_pkt_hdr));

	outb_back_p(ENISR_RDC, nic_base + EN0_ISR);	/* Ack intr. */
	ei_status.dmaing &= ~0x01;

	//printf("\noeuoeu: %x, %x, %x, %d\n", ring_page, hdr->status, hdr->next, hdr->count);
	le16_to_cpus(&hdr->count);
}

/* Block input and output, similar to the Crynwr packet driver.  If you
   are porting to a new ethercard, look at the packet driver source for hints.
   The NEx000 doesn't share the on-board packet memory -- you have to put
   the packet out through the "remote DMA" dataport using outb. */

void ne_block_input(struct net_device *dev, int count, char *buf, int ring_offset)
{
#ifdef NE_SANITY_CHECK
	int xfer_count = count;
#endif
	int nic_base = dev->base_addr;

	/* This *shouldn't* happen. If it does, it's the last thing you'll see */
	if(ei_status.dmaing)
	{
		printf("%s: DMAing conflict in ne_block_input "
			"[DMAstat:%d][irqlock:%d].\n",
			dev->name, ei_status.dmaing, ei_status.irqlock);
		return;
	}
	ei_status.dmaing |= 0x01;
	outb_back_p(E8390_NODMA+E8390_PAGE0+E8390_START, nic_base+ NE_CMD);
	outb_back_p(count & 0xff, nic_base + EN0_RCNTLO);
	outb_back_p(count >> 8, nic_base + EN0_RCNTHI);
	outb_back_p(ring_offset & 0xff, nic_base + EN0_RSARLO);
	outb_back_p(ring_offset >> 8, nic_base + EN0_RSARHI);
	outb_back_p(E8390_RREAD+E8390_START, nic_base + NE_CMD);
	if(ei_status.word16)
	{
		insw(NE_BASE + NE_DATAPORT,buf,count>>1);
		if(count & 0x01)
		{
			buf[count-1] = inb(NE_BASE + NE_DATAPORT);
#ifdef NE_SANITY_CHECK
			xfer_count++;
#endif
		}
	}
	else
		insb(NE_BASE + NE_DATAPORT, buf, count);

#ifdef NE_SANITY_CHECK
	/* This was for the ALPHA version only, but enough people have
	   been encountering problems so it is still here.  If you see
	   this message you either 1) have a slightly incompatible clone
	   or 2) have noise/speed problems with your bus. */

	if(ei_debug > 1)
	{
		/* DMA termination address check... */
		int addr, tries = 20;
		do {
			/* DON'T check for 'inb_p(EN0_ISR) & ENISR_RDC' here
			   -- it's broken for Rx on some cards! */
			int high = inb_p(nic_base + EN0_RSARHI);
			int low = inb_p(nic_base + EN0_RSARLO);
			addr = (high << 8) + low;
			if(((ring_offset + xfer_count) & 0xff) == low)
				break;
		} while(--tries > 0);
	 	if(tries <= 0)
			printf("%s: RX transfer address mismatch, %x (expected) vs. %x (actual).\n", dev->name, ring_offset + xfer_count, addr);
	}
#endif
	outb_back_p(ENISR_RDC, nic_base + EN0_ISR);	/* Ack intr. */
	ei_status.dmaing &= ~0x01;
}

void ne_block_output(struct net_device *dev, int count,
		const unsigned char *buf, const int start_page)
{
	int nic_base = NE_BASE;
	unsigned long dma_start;
#ifdef NE_SANITY_CHECK
	int retries = 0;
#endif

	/* Round the count up for word writes.  Do we need to do this?
	   What effect will an odd byte count have on the 8390?
	   I should check someday. */

	if(ei_status.word16 && (count & 0x01))
		count++;

	/* This *shouldn't* happen. If it does, it's the last thing you'll see */
	if(ei_status.dmaing)
	{
		printf("%s: DMAing conflict in ne_block_output. [DMAstat:%d][irqlock:%d]\n", dev->name, ei_status.dmaing, ei_status.irqlock);
		return;
	}
	ei_status.dmaing |= 0x01;
	/* We should already be in page 0, but to be safe... */
	outb_back_p(E8390_PAGE0+E8390_START+E8390_NODMA, nic_base + NE_CMD);

#ifdef NE_SANITY_CHECK
retry:
#endif

#ifdef NE8390_RW_BUGFIX
	/* Handle the read-before-write bug the same way as the
	   Crynwr packet driver -- the NatSemi method doesn't work.
	   Actually this doesn't always work either, but if you have
	   problems with your NEx000 this is better than nothing! */

	outb_back_p(0x42, nic_base + EN0_RCNTLO);
	outb_back_p(0x00,   nic_base + EN0_RCNTHI);
	outb_back_p(0x42, nic_base + EN0_RSARLO);
	outb_back_p(0x00, nic_base + EN0_RSARHI);
	outb_back_p(E8390_RREAD+E8390_START, nic_base + NE_CMD);
	/* Make certain that the dummy read has occurred. */
	udelay(6);
#endif

	outb_back_p(ENISR_RDC, nic_base + EN0_ISR);

	/* Now the normal output. */
	outb_back_p(count & 0xff, nic_base + EN0_RCNTLO);
	outb_back_p(count >> 8,   nic_base + EN0_RCNTHI);
	outb_back_p(0x00, nic_base + EN0_RSARLO);
	outb_back_p(start_page, nic_base + EN0_RSARHI);

	outb_back_p(E8390_RWRITE+E8390_START, nic_base + NE_CMD);
	if(ei_status.word16)
		outsw(NE_BASE + NE_DATAPORT, buf, count>>1);
	else
		outsb(NE_BASE + NE_DATAPORT, buf, count);

	dma_start = jiffies;

#ifdef NE_SANITY_CHECK
	/* This was for the ALPHA version only, but enough people have
	   been encountering problems so it is still here. */

	if(ei_debug > 1)
	{
		/* DMA termination address check... */
		int addr, tries = 20;
		do {
			int high = inb_p(nic_base + EN0_RSARHI);
			int low = inb_p(nic_base + EN0_RSARLO);
			addr = (high << 8) + low;
			if((start_page << 8) + count == addr)
				break;
		} while(--tries > 0);

		if(tries <= 0)
		{
			printf("%s: Tx packet transfer address mismatch, %x (expected) vs. %x (actual).\n", dev->name, (start_page << 8) + count, addr);
			if(retries++ == 0)
				goto retry;
		}
	}
#endif

	while((inb_p(nic_base + EN0_ISR) & ENISR_RDC) == 0)
		if(jiffies - dma_start > 2) // 20ms
		{
			printf("%s: timeout waiting for Tx RDC.\n", dev->name);
			ne_reset_8390(dev);
			NS8390_init(dev,1);
			break;
		}
	
	outb_back_p(ENISR_RDC, nic_base + EN0_ISR);	/* Ack intr. */
	ei_status.dmaing &= ~0x01;
	return;
}

