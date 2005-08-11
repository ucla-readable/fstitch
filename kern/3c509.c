#include <inc/x86.h>
#include <inc/stdio.h>
#include <inc/error.h>
#include <inc/josnic.h>
#include <inc/string.h>

#include <kern/3c509.h>
#include <kern/trap.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/josnic.h>

static int el3_debug = 2;

/* To minimize the size of the driver source I only define operating constants
 * if they are used several times. You'll need the manual anyway if you want to
 * understand driver details. */
/* Offsets from base I/O address. */
#define EL3_DATA 0x00
#define EL3_CMD 0x0e
#define EL3_STATUS 0x0E
#define	EEPROM_READ 0x80

#define EL3_IO_EXTENT 16

#define EL3WINDOW(win_num) outw(ioaddr + EL3_CMD, SelectWindow + (win_num))

/* The top five bits written to EL3_CMD are a command, the lower 11 bits are the
 * parameter, if applicable. */
enum c509cmd
{
	TotalReset = 0<<11, SelectWindow = 1<<11, StartCoax = 2<<11,
	RxDisable = 3<<11, RxEnable = 4<<11, RxReset = 5<<11, RxDiscard = 8<<11,
	TxEnable = 9<<11, TxDisable = 10<<11, TxReset = 11<<11,
	FakeIntr = 12<<11, AckIntr = 13<<11, SetIntrEnb = 14<<11,
	SetStatusEnb = 15<<11, SetRxFilter = 16<<11, SetRxThreshold = 17<<11,
	SetTxThreshold = 18<<11, SetTxStart = 19<<11, StatsEnable = 21<<11,
	StatsDisable = 22<<11, StopCoax = 23<<11, PowerUp = 27<<11,
	PowerDown = 28<<11, PowerAuto = 29<<11
};

enum c509status
{
	IntLatch = 0x0001, AdapterFailure = 0x0002, TxComplete = 0x0004,
	TxAvailable = 0x0008, RxComplete = 0x0010, RxEarly = 0x0020,
	IntReq = 0x0040, StatsFull = 0x0080, CmdBusy = 0x1000
};

/* The SetRxFilter command accepts the following classes: */
enum RxFilter
{
	RxStation = 1, RxMulticast = 2, RxBroadcast = 4, RxProm = 8
};

/* Register window 1 offsets, the window used in normal operation. */
#define TX_FIFO		0x00
#define RX_FIFO		0x00
#define RX_STATUS 	0x08
#define TX_STATUS 	0x0B
#define TX_FREE		0x0C		/* Remaining free bytes in Tx buffer. */

#define WN0_CONF_CTRL	0x04		/* Window 0: Configuration control register */
#define WN0_ADDR_CONF	0x06		/* Window 0: Address configuration register */
#define WN0_IRQ		0x08		/* Window 0: Set IRQ line in bits 12-15. */
#define WN4_MEDIA	0x0A		/* Window 4: Various transcvr/media bits. */
#define	MEDIA_TP	0x00C0		/* Enable link beat and jabber for 10baseT. */
#define WN4_NETDIAG	0x06		/* Window 4: Net diagnostic */
#define FD_ENABLE	0x8000		/* Enable full-duplex ("external loopback") */  

/* Time in jiffies before concluding the transmitter is hung. */
#define TX_TIMEOUT (40 * HZ / 100)

/* Number of 3c509 cards supported by this driver. */
#define MAX_EL3_DEVS 4

/* Maximum events (Rx packets, etc.) to handle at each interrupt. */
#define MAX_INTERRUPT_WORK 10

static struct {
	uint16_t valid:1, enabled:1, ready:1;
	uint8_t phys_addr[6];
	int base_addr, irq;
	int if_port, which;
} el3_dev[MAX_EL3_DEVS];
static int el3_devs = 0;

/* Read a word from the EEPROM when in the ISA ID probe state. */
static uint16_t id_read_eeprom(int id_port, int index)
{
	int bit, word = 0;
	
	/* Issue read command, and pause for at least 162 us. for it to complete.
	 * Assume extra-fast 16Mhz bus. */
	outb(id_port, EEPROM_READ + index);
	
	/* Pause for at least 162 us. for the read to take place. */
	kclock_delay(2);
	
	for(bit = 15; bit >= 0; bit--)
		word = (word << 1) + (inb(id_port) & 0x01);
	
	if(el3_debug > 3)
		printf("3c509 EEPROM word %d 0x%04x\n", index, word);
	
	return word;
}

static int el3_probe(const struct josnic * nic)
{
	static int current_tag = 0;
	/* Start with 0x110 to avoid new sound cards. */
	static int id_port = 0x110;
	
	short lrs_state = 0xFF, i;
	int ioaddr;

#if !ENABLE_INKERNEL_INTS
	printf("3c509: not probing, requires in-kernel interrupts\n");
	return -E_NO_DEV;
#else
	
	/* Select an open I/O location at 0x1*0 to do contention select. */
	for(; id_port < 0x200; id_port += 0x10)
	{
		outb(id_port, 0x00);
		outb(id_port, 0xFF);
		if(inb(id_port) & 0x01)
			break;
	}
	if(id_port >= 0x200)
	{
		/* Rare - do we really need a warning? */
		printf("WARNING: No I/O port available for 3c509 activation\n");
		return -E_NO_DEV;
	}
	
	/* Check for all ISA bus boards by sending the ID sequence to the
	 * ID_PORT. We find cards past the first by setting the 'current_tag' on
	 * cards as they are found. Cards with their tag set will not respond to
	 * subsequent ID sequences. */
	outb(id_port, 0x00);
	outb(id_port, 0x00);
	for(i = 0; i < 255; i++)
	{
		outb(id_port, lrs_state);
		lrs_state <<= 1;
		lrs_state = lrs_state & 0x100 ? lrs_state ^ 0xCF : lrs_state;
	}
	
	/* For the first probe, clear all board's tag registers. */
	if(!current_tag)
		outb(id_port, 0xD0);
	else				/* Otherwise kill off already-found boards. */
		outb(id_port, 0xD8);
	
	if(id_read_eeprom(id_port, 7) != 0x6D50)
		return -E_NO_DEV;
	
	/* Read in EEPROM data, which does contention-select.
	 * Only the lowest address board will stay "on-line".
	 * 3Com got the byte order backwards. */
	for(i = 0; i < 3; i++)
		((uint16_t *) &el3_dev[el3_devs].phys_addr)[i] = htons(id_read_eeprom(id_port, i));
	
	{
		unsigned int iobase = id_read_eeprom(id_port, 8);
		el3_dev[el3_devs].if_port = iobase >> 14;
		el3_dev[el3_devs].base_addr = ioaddr = 0x200 + ((iobase & 0x1F) << 4);
	}
	el3_dev[el3_devs].irq = id_read_eeprom(id_port, 9) >> 12;
	
	/* Set the adaptor tag so that the next card can be found. */
	outb(id_port, 0xD0 + ++current_tag);
	
	//if(!request_region(ioaddr, EL3_IO_EXTENT, "3c509"))
	//	return -E_BUSY;
	
	/* Activate the adaptor at the EEPROM location. */
	outb(id_port, (ioaddr >> 4) | 0xE0);
	
	EL3WINDOW(0);
	if(inw(ioaddr) != 0x6D50)
		return -E_NO_DEV;
	
	/* Free the interrupt so that some other card can use it. */
	outw(ioaddr + WN0_IRQ, 0x0F00);
	
	el3_dev[el3_devs].which = josnic_register(nic, el3_devs);
	if(el3_dev[el3_devs].which < 0)
		return -E_BUSY;
	
	{
		const char * if_names[] = {"10baseT", "AUI", "undefined", "BNC"};
		printf("eth%d: 3c509 at 0x%03x, %s port, address", el3_devs, el3_dev[el3_devs].base_addr, if_names[el3_dev[el3_devs].if_port & 0x03]);
	}
	
	/* Read in the station address. */
	for(i = 0; i < 6; i++)
		printf(" %02x", el3_dev[el3_devs].phys_addr[i]);
	printf(", IRQ %d\n", el3_dev[el3_devs].irq);
	
	el3_dev[el3_devs++].valid = 1;
	
	return 0;
#endif
}

/* Read a word from the EEPROM using the regular EEPROM access register.
 * Assume that we are in register window zero. */
static uint16_t read_eeprom(int ioaddr, int index)
{
	outw(ioaddr + 10, EEPROM_READ + index);
	/* Pause for at least 162 us. for the read to take place. */
	kclock_delay(2);
	return inw(ioaddr + 12);
}

static void el3_up(int which)
{
	int i, ioaddr = el3_dev[which].base_addr;
	
	if(!el3_dev[which].valid)
		return;
	
	/* Activating the board required and does no harm otherwise. */
	outw(ioaddr + 4, 0x0001);
	
	/* Set the IRQ line. */
	outw(ioaddr + WN0_IRQ, (el3_dev[which].irq << 12) | 0x0F00);
	
	/* Set the station address in window 2 each time opened. */
	EL3WINDOW(2);
	
	for(i = 0; i < 6; i++)
		outb(ioaddr + i, el3_dev[which].phys_addr[i]);
	
	if((el3_dev[which].if_port & 0x03) == 3) /* BNC interface */
		/* Start the thinnet transceiver. We should really wait 50ms...*/
		outw(ioaddr + EL3_CMD, StartCoax);
	else if((el3_dev[which].if_port & 0x03) == 0) /* 10baseT interface */
	{
		int sw_info, net_diag;
		/* Combine secondary sw_info word (the adapter level) and primary
		 * sw_info word (duplex setting plus other useless bits) */
		EL3WINDOW(0);
		sw_info = (read_eeprom(ioaddr, 0x14) & 0x400F) | (read_eeprom(ioaddr, 0x0D) & 0xBFF0);
		
		EL3WINDOW(4);
		net_diag = inw(ioaddr + WN4_NETDIAG);
		/* temporarily assume full-duplex will be set */
		net_diag |= FD_ENABLE;
		printf("eth%d: ", el3_dev[which].which);
		switch(el3_dev[which].if_port & 0x0C)
		{
			case 12:
				/* force full-duplex mode if 3c5x9b */
				if(sw_info & 0x000F)
				{
					printf("Forcing 3c5x9b full-duplex mode");
					break;
				}
			case 8:
				/* set full-duplex mode based on eeprom config setting */
				if((sw_info & 0x000F) && (sw_info & 0x8000))
				{
					printf("Setting 3c5x9b full-duplex mode (from EEPROM configuration bit)");
					break;
				}
			default:
				/* xcvr=(0 || 4) OR user has an old 3c509 non "B" model */
				printf("Setting 3c509 half-duplex mode");
				/* disable full duplex */
				net_diag &= ~FD_ENABLE;
		}
		
		outw(ioaddr + WN4_NETDIAG, net_diag);
		printf(" if_port: %d, sw_info: 0x%04x\n", el3_dev[which].if_port, sw_info);
		if(el3_debug > 3)
			printf("eth%d: 3c509 net diag word is now: 0x%04x\n", el3_dev[which].which, net_diag);
		/* Enable link beat and jabber check. */
		outw(ioaddr + WN4_MEDIA, inw(ioaddr + WN4_MEDIA) | MEDIA_TP);
	}
	
	/* Switch to the stats window, and clear all stats by reading. */
	outw(ioaddr + EL3_CMD, StatsDisable);
	EL3WINDOW(6);
	for(i = 0; i < 9; i++)
		inb(ioaddr + i);
	inw(ioaddr + 10);
	inw(ioaddr + 12);
	
	/* Switch to register set 1 for normal use. */
	EL3WINDOW(1);
	
	/* Accept b-cast and phys addr only. */
	outw(ioaddr + EL3_CMD, SetRxFilter | RxStation | RxBroadcast);
	outw(ioaddr + EL3_CMD, StatsEnable); /* Turn on statistics. */
	
	outw(ioaddr + EL3_CMD, RxEnable); /* Enable the receiver. */
	outw(ioaddr + EL3_CMD, TxEnable); /* Enable transmitter. */
	/* Allow status bits to be seen. */
	outw(ioaddr + EL3_CMD, SetStatusEnb | 0xFF);
	/* Ack all pending events, and set active indicator mask. */
	outw(ioaddr + EL3_CMD, AckIntr | IntLatch | TxAvailable | RxEarly | IntReq);
	outw(ioaddr + EL3_CMD, SetIntrEnb | IntLatch | TxAvailable | TxComplete | RxComplete | StatsFull);
	
	el3_dev[which].enabled = 1;
	el3_dev[which].ready = 1;
}

static int el3_start_xmit(int which, const void * data, int length)
{
	int ioaddr = el3_dev[which].base_addr;
	
	if(el3_debug > 4)
		printf("eth%d: %s(length = %u) called, status 0x%04x\n", el3_dev[which].which, __FUNCTION__, length, inw(ioaddr + EL3_STATUS));
	
	if(!el3_dev[which].ready)
		return -E_BUSY;
	
	/* Put out the doubleword header... */
	outw(ioaddr + TX_FIFO, length);
	outw(ioaddr + TX_FIFO, 0x00);
	/* ... and the packet rounded to a doubleword. */
	outsl(ioaddr + TX_FIFO, data, (length + 3) >> 2);
	
	if(inw(ioaddr + TX_FREE) <= 1536)
	{
		/* Interrupt us when the FIFO has room for max-sized packet. */
		outw(ioaddr + EL3_CMD, SetTxThreshold + 1536);
		el3_dev[which].ready = 0;
	}
	
	/* Clear the Tx status stack. */
	{
		short tx_status;
		int i = 4;
		
		while(--i > 0 && (tx_status = inb(ioaddr + TX_STATUS)) > 0)
		{
			//if(tx_status & 0x38)
			//	lp->stats.tx_aborted_errors++;
			if(tx_status & 0x30)
				outw(ioaddr + EL3_CMD, TxReset);
			if(tx_status & 0x3C)
				outw(ioaddr + EL3_CMD, TxEnable);
			/* Pop the status stack. */
			outb(ioaddr + TX_STATUS, 0x00);
		}
	}
	
	return 0;
}

/* Called asynchronously with interrupts disabled. */
static int el3_rx(int which)
{
	int ioaddr = el3_dev[which].base_addr;
	short rx_status;
	
	if(el3_debug > 5)
		printf("   In el3_rx(), status 0x%04x, rx_status 0x%04x\n", inw(ioaddr + EL3_STATUS), inw(ioaddr + RX_STATUS));
	while((rx_status = inw(ioaddr + RX_STATUS)) > 0)
	{
		if(rx_status & 0x4000)
		{
			/* Error, update stats. */
			//short error = rx_status & 0x3800;
			
			outw(ioaddr + EL3_CMD, RxDiscard);
			/*lp->stats.rx_errors++;
			switch(error)
			{
				case 0x0000:
					lp->stats.rx_over_errors++;
					break;
				case 0x0800:
					lp->stats.rx_length_errors++;
					break;
				case 0x1000:
					lp->stats.rx_frame_errors++;
					break;
				case 0x1800:
					lp->stats.rx_length_errors++;
					break;
				case 0x2000:
					lp->stats.rx_frame_errors++;
					break;
				case 0x2800:
					lp->stats.rx_crc_errors++;
					break;
			}*/
		}
		else
		{
			short pkt_len = rx_status & 0x7FF;
			void * buffer;
			
			//lp->stats.rx_bytes += pkt_len;
			if(el3_debug > 4)
				printf("Receiving packet size %d status 0x%04x\n", pkt_len, rx_status);
			
			buffer = josnic_async_push_packet(el3_dev[which].which, pkt_len);
			if(buffer)
			{
				insl(ioaddr + RX_FIFO, buffer, (pkt_len + 3) >> 2);
				/* Pop top Rx packet. */
				outw(ioaddr + EL3_CMD, RxDiscard);
				continue;
			}
			
			outw(ioaddr + EL3_CMD, RxDiscard);
			//lp->stats.rx_dropped++;
			if(el3_debug)
				printf("eth%d: Couldn't allocate a packet buffer of size %d\n", el3_dev[which].which, pkt_len);
		}
		/* Delay. */
		inw(ioaddr + EL3_STATUS);
		while(inw(ioaddr + EL3_STATUS) & 0x1000)
			printf("eth%d: Waiting for 3c509 to discard packet, status 0x%04x\n", el3_dev[which].which, inw(ioaddr + EL3_STATUS));
	}
	
	return 0;
}

/* Update statistics.  We change to register window 6, so this should be run
 * single-threaded if the device is active. This is expected to be a rare
 * operation, and it's simpler for the rest of the driver to assume that window
 * 1 is always valid rather than use a special window-state variable. */
static void update_stats(int which)
{
	int ioaddr = el3_dev[which].base_addr;
	
	if(el3_debug > 5)
		printf("eth%d: Updating the statistics\n", el3_dev[which].which);
	/* Turn off statistics updates while reading. */
	outw(ioaddr + EL3_CMD, StatsDisable);
	/* Switch to the stats window, and read everything. */
	EL3WINDOW(6);
	/*lp->stats.tx_carrier_errors 	+=*/ inb(ioaddr + 0);
	/*lp->stats.tx_heartbeat_errors	+=*/ inb(ioaddr + 1);
	/* Multiple collisions. */	   inb(ioaddr + 2);
	/*lp->stats.collisions		+=*/ inb(ioaddr + 3);
	/*lp->stats.tx_window_errors	+=*/ inb(ioaddr + 4);
	/*lp->stats.rx_fifo_errors	+=*/ inb(ioaddr + 5);
	/*lp->stats.tx_packets		+=*/ inb(ioaddr + 6);
	/* Rx packets	*/		   inb(ioaddr + 7);
	/* Tx deferrals */		   inb(ioaddr + 8);
	inw(ioaddr + 10);	/* Total Rx and Tx octets. */
	inw(ioaddr + 12);
	
	/* Back to window 1, and turn statistics back on. */
	EL3WINDOW(1);
	outw(ioaddr + EL3_CMD, StatsEnable);
}

/* The EL3 interrupt handler. Called asynchronously with interrupts disabled. */
static void el3_intr(int irq)
{
	int ioaddr, status;
	int i = MAX_INTERRUPT_WORK;
	int which;
	
	for(which = 0; which != el3_devs; which++)
		if(el3_dev[which].irq == irq)
			break;
	
	if(which == el3_devs)
	{
		printf("%s(): IRQ %d for unknown device\n", __FUNCTION__, irq);
		return;
	}
	
	ioaddr = el3_dev[which].base_addr;
	
	if(el3_debug > 4)
	{
		status = inw(ioaddr + EL3_STATUS);
		printf("eth%d: interrupt, status 0x%04x\n", el3_dev[which].which, status);
	}
	
	while((status = inw(ioaddr + EL3_STATUS)) & (IntLatch | RxComplete | StatsFull))
	{
		if(status & RxComplete)
			el3_rx(which);
		
		if(status & TxAvailable)
		{
			if(el3_debug > 5)
				printf("	TX room bit was handled\n");
			/* There's room in the FIFO for a max-sized packet. */
			outw(ioaddr + EL3_CMD, AckIntr | TxAvailable);
			el3_dev[which].ready = 1;
		}
		
		/* Handle all uncommon interrupts. */
		if(status & (AdapterFailure | RxEarly | StatsFull | TxComplete))
		{
			if(status & StatsFull)
				/* Empty statistics. */
				update_stats(which);
			if(status & RxEarly)
			{
				/* Rx early is unused. */
				el3_rx(which);
				outw(ioaddr + EL3_CMD, AckIntr | RxEarly);
			}
			if(status & TxComplete)
			{
				/* Really Tx error. */
				short tx_status;
				int i = 4;
				
				while (--i > 0 && (tx_status = inb(ioaddr + TX_STATUS)) > 0)
				{
					//if(tx_status & 0x38)
					//	lp->stats.tx_aborted_errors++;
					if(tx_status & 0x30)
						outw(ioaddr + EL3_CMD, TxReset);
					if(tx_status & 0x3C)
						outw(ioaddr + EL3_CMD, TxEnable);
					/* Pop the status stack. */
					outb(ioaddr + TX_STATUS, 0x00);
				}
			}
			if(status & AdapterFailure)
			{
				/* Adapter failure requires Rx reset and reinit. */
				outw(ioaddr + EL3_CMD, RxReset);
				/* Set the Rx filter to the current state. */
				//outw(ioaddr + EL3_CMD, SetRxFilter | RxStation | RxBroadcast | (dev[which].flags & IFF_ALLMULTI ? RxMulticast : 0) | (dev[which].flags & IFF_PROMISC ? RxProm : 0));
				outw(ioaddr + EL3_CMD, SetRxFilter | RxStation | RxBroadcast);
				outw(ioaddr + EL3_CMD, RxEnable); /* Re-enable the receiver. */
				outw(ioaddr + EL3_CMD, AckIntr | AdapterFailure);
			}
		}
		
		if(--i < 0)
		{
			printf("eth%d: Infinite loop in interrupt, status 0x%04x\n", el3_dev[which].which, status);
			/* Clear all interrupts. */
			outw(ioaddr + EL3_CMD, AckIntr | 0xFF);
			break;
		}
		/* Acknowledge the IRQ. */
		outw(ioaddr + EL3_CMD, AckIntr | IntReq | IntLatch);
	}
	
	if(el3_debug > 4)
		printf("eth%d: exiting interrupt, status 0x%04x\n", el3_dev[which].which, inw(ioaddr + EL3_STATUS));
}

static int el3_open(int which)
{
	int ioaddr = el3_dev[which].base_addr;
	
	outw(ioaddr + EL3_CMD, TxReset);
	outw(ioaddr + EL3_CMD, RxReset);
	outw(ioaddr + EL3_CMD, SetStatusEnb | 0x00);
	
	if(request_irq(el3_dev[which].irq, el3_intr))
		return -1;
	irq_setmask_8259A(irq_mask_8259A & ~(1 << el3_dev[which].irq));
	
	EL3WINDOW(0);
	if(el3_debug > 3)
		printf("eth%d: Opening, IRQ %d status@%x 0x%04x\n", el3_dev[which].which, el3_dev[which].irq, ioaddr + EL3_STATUS, inw(ioaddr + EL3_STATUS));
	
	el3_up(which);
	
	if(el3_debug > 3)
		printf("eth%d: Opened 3c509 IRQ %d status 0x%04x\n", el3_dev[which].which, el3_dev[which].irq, inw(ioaddr + EL3_STATUS));
	
	return 0;
}

static void el3_down(int which)
{
	int ioaddr = el3_dev[which].base_addr;
	
	/* Turn off statistics ASAP. */
	outw(ioaddr + EL3_CMD, StatsDisable);
	
	/* Disable the receiver and transmitter. */
	outw(ioaddr + EL3_CMD, RxDisable);
	outw(ioaddr + EL3_CMD, TxDisable);
	
	if(el3_dev[which].if_port == 3)
		/* Turn off thinnet power. Green! */
		outw(ioaddr + EL3_CMD, StopCoax);
	else if(el3_dev[which].if_port == 0)
	{
		/* Disable link beat and jabber, if_port may change here next open(). */
		EL3WINDOW(4);
		outw(ioaddr + WN4_MEDIA, inw(ioaddr + WN4_MEDIA) & ~MEDIA_TP);
	}
	
	outw(ioaddr + EL3_CMD, SetIntrEnb | 0x0000);
	
	update_stats(which);
}

static int el3_close(int which)
{
	int ioaddr = el3_dev[which].base_addr;
	
	if(el3_debug > 2)
		printf("eth%d: Shutting down ethercard\n", el3_dev[which].which);
	
	el3_down(which);
	
	irq_setmask_8259A(irq_mask_8259A | (1 << el3_dev[which].irq));
	request_irq(el3_dev[which].irq, NULL);
	
	/* Switching back to window 0 disables the IRQ. */
	EL3WINDOW(0);
	/* But we explicitly zero the IRQ line select anyway. */
	outw(ioaddr + WN0_IRQ, 0x0F00);
	
	el3_dev[which].ready = 0;
	el3_dev[which].enabled = 0;
	
	return 0;
}

static int el3_link_ok(int which)
{
	int ioaddr = el3_dev[which].base_addr;
	register_t eflags = read_eflags();
	uint16_t tmp;
	
	__asm__ __volatile__("cli");
	
	EL3WINDOW(4);
	tmp = inw(ioaddr + WN4_MEDIA);
	EL3WINDOW(1);
	
	write_eflags(eflags);
	
	return tmp & (1 << 11);
}

static int el3_tx_timeout(int which)
{
	int ioaddr;
	
	if(which < 0)
		return -E_INVAL;
	if(el3_devs <= which || !el3_dev[which].valid)
		return -E_NO_DEV;
	if(!el3_dev[which].enabled)
		return -E_BUSY;
	
	ioaddr = el3_dev[which].base_addr;
	
	/* Transmitter timeout, serious problems. */
	printf("eth%d: transmit timed out, Tx_status 0x%02x status 0x%04x Tx FIFO room %d\n", el3_dev[which].which, inb(ioaddr + TX_STATUS), inw(ioaddr + EL3_STATUS), inw(ioaddr + TX_FREE));
	
	//lp->stats.tx_errors++;
	
	/* Issue TX_RESET and TX_START commands. */
	outw(ioaddr + EL3_CMD, TxReset);
	outw(ioaddr + EL3_CMD, TxEnable);
	el3_dev[which].ready = 1;
	
	return 0;
}

static int el3_get_address(int which, uint8_t * buffer)
{
	if(which < 0)
		return -E_INVAL;
	if(el3_devs <= which || !el3_dev[which].valid)
		return -E_NO_DEV;
	if(!el3_dev[which].enabled)
		return -E_BUSY;
	
	memcpy(buffer, el3_dev[which].phys_addr, sizeof(el3_dev[which].phys_addr));
	return 0;
}

static int el3_set_filter(int which, int flags)
{
	if(which < 0)
		return -E_INVAL;
	if(el3_devs <= which || !el3_dev[which].valid)
		return -E_NO_DEV;
	if(!el3_dev[which].enabled)
		return -E_BUSY;
	
	return -1;
}

static const struct josnic el3_nic = {open: el3_open, close: el3_close, address: el3_get_address, transmit: el3_start_xmit, filter: el3_set_filter, reset: el3_tx_timeout};

int el3_init(void)
{
	int i;
	for(i = 0; i != MAX_EL3_DEVS; i++)
		if(el3_probe(&el3_nic) == -E_NO_DEV)
			break;
	printf("3c509: detected %d cards\n", el3_devs);
	return i ? 0 : -E_NO_DEV;
}


/* below here are things left over from the linux driver */
#if 0

/*
 *     Set or clear the multicast filter for this adaptor.
 */
static void
set_multicast_list(struct net_device *dev)
{
	unsigned long flags;
	struct el3_private *lp = (struct el3_private *)dev->priv;
	int ioaddr = dev->base_addr;
	
	if (el3_debug > 1) {
		static int old;
		if (old != dev->mc_count) {
			old = dev->mc_count;
			printf("%s: Setting Rx mode to %d addresses\n", dev->name, dev->mc_count);
		}
	}
	spin_lock_irqsave(&lp->lock, flags);
	if (dev->flags&IFF_PROMISC)
		outw(ioaddr + EL3_CMD, SetRxFilter | RxStation | RxMulticast | RxBroadcast | RxProm);
	else if (dev->mc_count || (dev->flags&IFF_ALLMULTI))
		outw(ioaddr + EL3_CMD, SetRxFilter | RxStation | RxMulticast | RxBroadcast);
	else
                outw(ioaddr + EL3_CMD, SetRxFilter | RxStation | RxBroadcast);
	spin_unlock_irqrestore(&lp->lock, flags);
}

static int
el3_netdev_get_ecmd(struct net_device *dev, struct ethtool_cmd *ecmd)
{
	u16 tmp;
	int ioaddr = dev->base_addr;
	
	EL3WINDOW(0);
	/* obtain current tranceiver via WN4_MEDIA? */	
	tmp = inw(ioaddr + WN0_ADDR_CONF);
	ecmd->transceiver = XCVR_INTERNAL;
	switch (tmp >> 14) {
	case 0:
		ecmd->port = PORT_TP;
		break;
	case 1:
		ecmd->port = PORT_AUI;
		ecmd->transceiver = XCVR_EXTERNAL;
		break;
	case 3:
		ecmd->port = PORT_BNC;
	default:
		break;
	}
	
	ecmd->duplex = DUPLEX_HALF;
	ecmd->supported = 0;
	tmp = inw(ioaddr + WN0_CONF_CTRL);
	if (tmp & (1<<13))
		ecmd->supported |= SUPPORTED_AUI;
	if (tmp & (1<<12))
		ecmd->supported |= SUPPORTED_BNC;
	if (tmp & (1<<9)) {
		ecmd->supported |= SUPPORTED_TP | SUPPORTED_10baseT_Half |
				SUPPORTED_10baseT_Full;	/* hmm... */
		EL3WINDOW(4);
		tmp = inw(ioaddr + WN4_NETDIAG);
		if (tmp & FD_ENABLE)
			ecmd->duplex = DUPLEX_FULL;
	}
	
	ecmd->speed = SPEED_10;
	EL3WINDOW(1);
	return 0;
}

static int
el3_netdev_set_ecmd(struct net_device *dev, struct ethtool_cmd *ecmd)
{
	u16 tmp;
	int ioaddr = dev->base_addr;
	
	if (ecmd->speed != SPEED_10)
		return -EINVAL;
	if ((ecmd->duplex != DUPLEX_HALF) && (ecmd->duplex != DUPLEX_FULL))
		return -EINVAL;
	if ((ecmd->transceiver != XCVR_INTERNAL) && (ecmd->transceiver != XCVR_EXTERNAL))
		return -EINVAL;
	
	/* change XCVR type */
	EL3WINDOW(0);
	tmp = inw(ioaddr + WN0_ADDR_CONF);
	switch (ecmd->port) {
	case PORT_TP:
		tmp &= ~(3<<14);
		dev->if_port = 0;
		break;
	case PORT_AUI:
		tmp |= (1<<14);
		dev->if_port = 1;
		break;
	case PORT_BNC:
		tmp |= (3<<14);
		dev->if_port = 3;
		break;
	default:
		return -EINVAL;
	}
	
	outw(ioaddr + WN0_ADDR_CONF, tmp);
	if (dev->if_port == 3) {
		/* fire up the DC-DC convertor if BNC gets enabled */
		tmp = inw(ioaddr + WN0_ADDR_CONF);
		if (tmp & (3 << 14)) {
			outw(ioaddr + EL3_CMD, StartCoax);
			udelay(800);
		} else
			return -EIO;
	}
	
	EL3WINDOW(4);
	tmp = inw(ioaddr + WN4_NETDIAG);
	if (ecmd->duplex == DUPLEX_FULL)
		tmp |= FD_ENABLE;
	else
		tmp &= ~FD_ENABLE;
	outw(ioaddr + WN4_NETDIAG, tmp);
	EL3WINDOW(1);
	
	return 0;
}

#endif
