/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/pmap.h>
#include <inc/kbdreg.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/env.h>
#include <inc/serial.h>
#include <inc/error.h>

#include <kern/console.h>
#include <kern/picirq.h>
#include <kern/kclock.h>
#include <kern/trap.h>
#include <kern/irq.h>
#include <kern/env.h>
#include <kern/serial.h>

static void cons_intr(int (*proc)(void));
static void serial_intr(int irq);


/***** Serial I/O code *****/

#define COM1 0x3F8
#define COMSTATUS 5
#define COMDATA 0x01
#define COMREADY 0x20
#define COMREAD 0
#define COMWRITE 0


struct Com com[NCOMS];


//
// struct Com utilities

// Returns user of the given port,
// 0 is free, -E_INVAL is port not present, otherwise the owner
envid_t
Com_user(int port)
{
	if(port >= NCOMS)
		return -E_INVAL;
	if(com[port].addr == 0)
		return -E_INVAL;

	if(com[port].user == ENVID_KERNEL)
		return ENVID_KERNEL;

	struct Env *tmp_env;
	if(envid2env(com[port].user, &tmp_env, 0))
		return 0;

	return com[port].user;
}

uint8_t
Com_irq(uint8_t port)
{
	switch(port)
	{
		case 0:
		case 2:
			return 4;
		case 1:
		case 3:
			return 3;
		default:
			return 0;
	}
}


// Returns index of console upon finding which, else returns NCOMS
static int
console_port_idx()
{
	int i;
	for(i=0; i<NCOMS; i++)
	{
		if(Com_user(i) == ENVID_KERNEL)
			break;
	}

	return i;
}


//
// Serial port setup

static void
detect_serial(void)
{
	// This function adapted from http://www.beyondlogic.org/serial/serial.htm

	uint16_t *ptraddr; // Pointer to location of Port Addresses
	uint16_t  address; // Address of Port
	int a, n = 0;
	
	/* can't use KADDR yet */
	ptraddr= (uint16_t*)(KERNBASE + 0x00000400);
	
	printf("Serial port discovery:");
	for (a = 0; a <  4; a++)
	{
		address = *ptraddr;
		com[a].addr = address;
		com[a].user = 0;

		if(address)
			printf("%s %d is 0x%x", n++ ? "," : "", a, address);

		*ptraddr++;
	}
	printf(".\n");
}

static void
serial_init_port(uint16_t com_addr, uint32_t speed)
{
	// Much of this code was guided/adapted from linux 2.6.6's
	// sound/drivers/serial-u16550.c.

	outb(com_addr+UART_FCR,
		  (UART_FCR_ENABLE_FIFO
			| UART_FCR_CLEAR_RCVR
			| UART_FCR_CLEAR_XMIT
			| UART_FCR_TRIGGER_4));
	
	const uint32_t rate_divisor = 115200;
	const uint32_t rate_desired =  speed;
	const uint8_t rate_register = rate_divisor / rate_desired; // what we store
	assert(rate_divisor == rate_register*rate_desired);
	
	outb(com_addr+UART_LCR, UART_LCR_DLAB);// DLAB on to set speed (thanks Mike)
	outb(com_addr+UART_DLL, rate_register); // latch low
	outb(com_addr+UART_DLM, 0x00);          // latch high
	
	outb(com_addr+UART_LCR,
		  (UART_LCR_WLEN8 // 8 data-bits
			| 0 // 1 stop-bit
			| 0 // parity off
			| 0)); // DLAB = 0
	
	outb(com_addr+UART_MCR,
		  (UART_MCR_RTS
			| UART_MCR_DTR
			| UART_MCR_OUT2));
	
	// Need to service these if we enable them
	outb(com_addr+UART_IER, UART_IER_RDI);// | UART_IER_THRI);
	
	inb(com_addr+UART_LSR); // Clear any pre-existing overrun indicom_addrtion
	inb(com_addr+UART_IIR); // Clear any pre-existing transmit interrupt
	inb(com_addr+UART_RX);  // Clear any pre-existing receive interrupt
}


static void
serial_init(void)
{
	// NOTE: 115200 can sometimes be too fast for running inside of bochs.
	// On real hardware, however, it should be fine.
	const uint32_t speed = 57600;

	detect_serial();

	bool console_port_set = 0;
	int i;
	for(i=0; i<NCOMS; i++)
	{
		if(com[i].addr != 0) {
			serial_init_port(com[i].addr, speed);

#if ENABLE_SERIAL_CONSOLE
			if(!console_port_set)
			{
				com[i].user = ENVID_KERNEL;
				console_port_set = 1;
				printf("Serial console enabled for port %d.\n", i);
			}
#endif
		}
	}


	if(com[0].addr || com[2].addr)
	{
		request_irq(4, serial_intr);
		irq_setmask_8259A(irq_mask_8259A & ~(1<<4));	
	}
	if(com[1].addr || com[4].addr)
	{
		request_irq(3, serial_intr);
		irq_setmask_8259A(irq_mask_8259A & ~(1<<3));	
	}
}


// Returns the next byte from the serial port, -1 if no data waiting
int
serial_getc(uint8_t port)
{
	uint16_t ca = com[port].addr;
	if(!ca)
		return -1;
	if (!(inb(ca+COMSTATUS) & COMDATA))
		return -1;

	uint8_t data = inb(ca+UART_RX);
	return data;
}

static int
serial_getc_console()
{
	int cons_idx = console_port_idx();

	if(cons_idx >= NCOMS)
		return -1; // kernel does not have a com for console

	// kernel has com i for console
	const int c = serial_getc(cons_idx);

	/* 24 == ^X */
	if(c == 24)
		reboot();
	/* readline() ignores '\r' */
	if(c == '\r')
		return '\n';

	return c;
}

static void
serial_getc_userspace(uint8_t port)
{
	if(Com_user(port) < 1)
	{
		// Since serial ports can share irqs, we may have been called for our
		// our brother port's irq. Thus don't display an error about spurious
		// data, even though this /might/ be spurious data.
		return;
	}

	uint32_t prev_pfm = page_fault_mode;
	page_fault_mode = PFM_KILL;

	uint8_t *buf = (uint8_t*) com[port].buf;
	uint16_t begin_idx = get_buf_begin(buf);
	uint16_t end_idx   = get_buf_end(buf);

	while(get_buf_free(begin_idx, end_idx) > 0)
	{
		int c = serial_getc(port);
		if(c == -1)
		{
			page_fault_mode = prev_pfm;
			return;
		}
		uint8_t *buf_c = &buf[end_idx];
		if(*buf_c != 0)
			panic("*buf_c = 0x%0x, end_idx = %d\n", *buf_c, end_idx);
		*buf_c = (uint8_t) c;
		end_idx = inc_buf_end(buf);
	}

	//printf("OUT OF BUFFER: serial port %d for envid 0x%x\n",
	//		 port, Com_user(port));

	// We must reset this interrupt so that we'll recv future interrupts.
	// Here we drain the port, it would be better to set RTS low and clear
	// the recv interrupt here and raise RTS when there is space again.
	// However, I've not gotten this to work yet.
	while (serial_getc(port) != -1);

	page_fault_mode = prev_pfm;
}

static void
serial_intr(int irq)
{
	switch(irq)
	{
		case(4):
			if(Com_user(0) == ENVID_KERNEL)
				cons_intr(serial_getc_console);
			else
				serial_getc_userspace(0);
		
			if(Com_user(2) == ENVID_KERNEL)
				cons_intr(serial_getc_console);
			else
				serial_getc_userspace(2);
			break;

		case(3):
			if(Com_user(1) == ENVID_KERNEL)
				cons_intr(serial_getc_console);
			else
				serial_getc_userspace(1);
			
			if(Com_user(3) == ENVID_KERNEL)
				cons_intr(serial_getc_console);
			else
				serial_getc_userspace(3);
			break;

		default:
			panic("serial_intr not written to handle irq %d", irq);
	}
}

static void
serial_putc(uint8_t c, uint8_t port)
{
	uint8_t thre;
	uint16_t ca = com[port].addr;
	if(ca == 0)
		return;

	const int uart_lsr = 5;
	while((thre = inb(com[port].addr+uart_lsr) & 0x20) == 0)
		{ }
	
	outb(com[port].addr+UART_TX, c);
}

static void
serial_putc_console(uint8_t c)
{
	int cons_idx = console_port_idx();
	if(cons_idx >= NCOMS)
		return;

	if(c == '\n')
	{
		// '\n' means new line.
		// '\r' means go to the beginning of the line.
		// Use *both* to go to the beginning of the next line.
		serial_putc('\r', cons_idx);
	}
	serial_putc(c, cons_idx);
}

/***** Parallel port output code *****/
// For information on PC parallel port programming, see the class References
// page.

// Stupid I/O delay routine necessitated by historical PC design flaws
static void
delay(void)
{
	inb(0x84);
	inb(0x84);
	inb(0x84);
	inb(0x84);
}

static void
lpt_putc(int c)
{
	int i;

	for (i=0; !(inb(0x378+1)&0x80) && i<12800; i++)
		delay();
	outb(0x378+0, c);
	outb(0x378+2, 0x08|0x01);
	outb(0x378+2, 0x08);
}


/***** Text-mode CGA/VGA display output *****/

static unsigned addr_6845;
static uint16_t* crt_buf;
static uint16_t crt_pos;

static void
cga_init(void)
{
	volatile uint16_t *cp;
	uint16_t was;
	unsigned pos;

	cp = (uint16_t*) (KERNBASE + CGA_BUF);
	was = *cp;
	*cp = (uint16_t) 0xA55A;
	if (*cp != 0xA55A) {
		cp = (uint16_t*) (KERNBASE + MONO_BUF);
		addr_6845 = MONO_BASE;
	} else {
		*cp = was;
		addr_6845 = CGA_BASE;
	}
	
	/* Extract cursor location */
	outb(addr_6845, 14);
	pos = inb(addr_6845 + 1) << 8;
	outb(addr_6845, 15);
	pos |= inb(addr_6845 + 1);

	crt_buf = (uint16_t*) cp;
	crt_pos = pos;
}


static void
cga_putc(int c)
{
	/* if no attribute given, then use black on white */
	if (!(c & ~0xff))
		c |= 0x0700;

	switch (c & 0xff) {
	case '\a':
		/* enable counter 2 */
		outb(0x61, inb(0x61) | 3);
		/* set command for counter 2, 2 byte write */
		outb(0x43, 0xB6);
		/* select desired HZ */
		outb(0x42, 0x36);
		outb(0x42, 0x06);
		
		kclock_delay(10);
		/* disable counter 2 */
		outb(0x61, inb(0x61) & 0xFC);
		break;
	case '\b':
		if (crt_pos > 0) {
			crt_pos--;
			crt_buf[crt_pos] = (c & ~0xff) | ' ';
		}
		break;
	/* backspace without erase */
	case 127:
		if (crt_pos > 0)
			crt_pos--;
		break;
	case '\n':
		crt_pos += CRT_COLS;
		/* fallthru */
	case '\r':
		crt_pos -= (crt_pos % CRT_COLS);
		break;
	case '\t':
		cons_putc(' ');
		cons_putc(' ');
		cons_putc(' ');
		cons_putc(' ');
		cons_putc(' ');
		break;
	default:
		crt_buf[crt_pos++] = c;		/* write the character */
		break;
	}

	// What is the purpose of this?
	if (crt_pos >= CRT_SIZE) {
		int i;
		memcpy(crt_buf, crt_buf + CRT_COLS, (CRT_SIZE - CRT_COLS) << 1);
		for (i = CRT_SIZE - CRT_COLS; i < CRT_SIZE; i++)
			crt_buf[i] = 0x0700 | ' ';
		crt_pos -= CRT_COLS;
	}

	/* move that little blinky thing */
	outb(addr_6845, 14);
	outb(addr_6845 + 1, crt_pos >> 8);
	outb(addr_6845, 15);
	outb(addr_6845 + 1, crt_pos);
}



/***** Keyboard input code *****/

#define NO		0

#define SHIFT		(1<<0)
#define CTL		(1<<1)
#define ALT		(1<<2)

#define SCROLLOCK	(1<<3)
#define NUMLOCK		(1<<4)
#define CAPSLOCK	(1<<5)
#define LOCKMASK	(SCROLLOCK | NUMLOCK | CAPSLOCK)
#define LOCKSHIFT	3

#define E0ESC		(1<<6)

static int shiftcode[128] = 
{
[29] CTL,
[42] SHIFT,
[54] SHIFT,
[56] ALT,
};

static int togglecode[128] = 
{
[58] CAPSLOCK,
[69] NUMLOCK,
[70] SCROLLOCK,
};

static unsigned char normalmap[128] =
{
	NO,    033,  '1',  '2',  '3',  '4',  '5',  '6',
	'7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',
	'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',
	'o',  'p',  '[',  ']',  '\n', NO,   'a',  's',
	'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',
	'\'', '`',  NO,   '\\', 'z',  'x',  'c',  'v',
	'b',  'n',  'm',  ',',  '.',  '/',  NO,   '*',
	NO,   ' ',   NO,   NO,   NO,   NO,   NO,   NO,
	NO,   NO,   NO,   NO,   NO,   NO,   NO,   '7',
	'8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
	'2',  '3',  '0',  '.',  
};

static unsigned char shiftmap[128] = 
{
	NO,   033,  '!',  '@',  '#',  '$',  '%',  '^',
	'&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
	'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
	'O',  'P',  '{',  '}',  '\n', NO,   'A',  'S',
	'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',
	'"',  '~',  NO,   '|',  'Z',  'X',  'C',  'V',
	'B',  'N',  'M',  '<',  '>',  '?',  NO,   '*',
	NO,   ' ',   NO,   NO,   NO,   NO,   NO,   NO,
	NO,   NO,   NO,   NO,   NO,   NO,   NO,   '7',
	'8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
	'2',  '3',  '0',  '.',  
};

#define C(x) (x-'@')
#define A(x) (C(x) | 0x80)

static unsigned char ctlmap[128] = 
{
	NO,      NO,      NO,      NO,      NO,      NO,      NO,      NO, 
	NO,      NO,      NO,      NO,      NO,      NO,      NO,      NO, 
	C('Q'),  C('W'),  C('E'),  C('R'),  C('T'),  C('Y'),  C('U'),  C('I'),
	C('O'),  C('P'),  NO,      NO,      '\r',    NO,      C('A'),  C('S'),
	C('D'),  C('F'),  C('G'),  C('H'),  C('J'),  C('K'),  C('L'),  NO, 
	NO,      NO,      NO,      C('\\'), C('Z'),  C('X'),  C('C'),  C('V'),
	C('B'),  C('N'),  C('M'),  NO,      NO,      C('/'),  NO,      NO, 
};

static unsigned char altmap[128] =
{
	NO,      NO,      NO,      NO,      NO,      NO,      NO,      NO, 
	NO,      NO,      NO,      NO,      NO,      NO,      NO,      NO, 
	A('Q'),  A('W'),  A('E'),  A('R'),  A('T'),  A('Y'),  A('U'),  A('I'),
	A('O'),  A('P'),  NO,      NO,      '\r',    NO,      A('A'),  A('S'),
	A('D'),  A('F'),  A('G'),  A('H'),  A('J'),  A('K'),  A('L'),  NO, 
	NO,      NO,      NO,      A('\\'), A('Z'),  A('X'),  A('C'),  A('V'),
	A('B'),  A('N'),  A('M'),  NO,      NO,      A('/'),  NO,      NO, 
};

static unsigned char intrmap[128] =
{
	[83] 255,
};

static unsigned char * charcode[8] =
{
	normalmap, /* no modifiers */
	shiftmap,  /* shift */
	ctlmap,    /* ctrl */
	ctlmap,    /* ctrl+shift */
	altmap,    /* alt */
	altmap,    /* alt+shift */
	intrmap,   /* ctrl+alt */
	intrmap,   /* ctrl+alt+shift */
};

/*
 * Get data from the keyboard.  If we finish a character, return it.  Else 0.
 * Return -1 if no data.
 */
static int
kbd_proc_data(void)
{
	int c;
	uint8_t data;
	static uint32_t shift = 0;

	if ((inb(KBSTATP) & KBS_DIB) == 0)
		return -1;

	data = inb(KBDATAP);

	if (data == KBR_EXTENDED) {
		shift |= E0ESC;
		return 0;
	}

	if (data & 0x80) {
		/* key up */
		shift &= ~(shiftcode[data&~0x80] | E0ESC);
		return 0;
	}

	/* key down */
	shift |= shiftcode[data];
	shift ^= togglecode[data];
	
	if(togglecode[data])
	{
		/* update the keyboard LEDs */
		while(inb(KBSTATP) & KBS_IBF);
		outb(KBDATAP, KBC_MODEIND);
		while(inb(KBSTATP) & KBS_IBF);
		outb(KBDATAP, (shift & LOCKMASK) >> LOCKSHIFT);
		return 0;
	}
	/* modifier keys have no effect except as modifiers... */
	if(shiftcode[data])
		return 0;
	
	c = charcode[shift&(ALT|CTL|SHIFT)][data];

	if (shift&E0ESC) {
		if (c == 255)
			reboot();
		shift &= ~E0ESC;
		return c | 0x80;
	}
	if (shift&CAPSLOCK) {
		if ('a' <= c && c <= 'z')
			c += 'A' - 'a';
		else if ('A' <= c && c <= 'Z')
			c += 'a' - 'A';
	}

	if (shift&CTL && c == C('Q')) {
		breakpoint();
		return -1; // eat this input
	}
	
	return c;
}

static void
kbd_intr(int irq)
{
	cons_intr(kbd_proc_data);
}

static void
kbd_init(void)
{
	// Drain the kbd buffer so that Bochs generates interrupts.
	kbd_intr(1);
	request_irq(1, kbd_intr);
	irq_setmask_8259A(irq_mask_8259A & ~(1<<1));
}



/***** General device-independent console code *****/
// Here we manage the console input buffer,
// where we stash characters received from the keyboard or serial port
// whenever the corresponding interrupt occurs.

#define BY2CONS 512

static struct {
	uint8_t buf[BY2CONS];
	uint16_t rpos;
	uint16_t wpos;
} cons;

// called by device interrupt routines to feed input characters
// into the circular console input buffer.
static void
cons_intr(int (*proc)(void))
{
	int c;

	while ((c = (*proc)()) != -1) {
		if (c == 0)
			continue;
		
		/* ^C */
		if(c == 3 && curenv) {
#if ENABLE_INKERNEL_INTS
			__asm__ __volatile__("sti"); // this might be inside an interrupt
#endif
			printf("[%08x] kill env %08x via ^C\n", curenv->env_id, curenv->env_id);
			env_destroy(curenv);
			/* env_destroy() does not return on curenv */
		}
		
		cons.buf[cons.wpos++] = c;
		if (cons.wpos == BY2CONS)
			cons.wpos = 0;
	}
}

// return the next input character from the console, or -1 if none waiting
int
cons_getc(void)
{
	int c;
	register_t eflags = read_eflags();

#if ENABLE_INKERNEL_INTS
	__asm__ __volatile__("cli");
#endif

	// poll for any pending input characters,
	// so that this function works even when interrupts are disabled
	// (e.g., when called from the kernel monitor).
#if ENABLE_SERIAL_CONSOLE
	{
		const int cons_idx = console_port_idx();
		if(cons_idx < NCOMS)
		{
			const int cons_irq = Com_irq(cons_idx);
			serial_intr(cons_irq);
		}
	}
#endif
	kbd_intr(1);
	write_eflags(eflags);

	// grab the next character from the input buffer.
	if (cons.rpos != cons.wpos) {
		c = cons.buf[cons.rpos++];
		if (cons.rpos == BY2CONS)
			cons.rpos = 0;
		return c;
	}
	return -1;
}

// output a character to the console
void
cons_putc(int c)
{
#if ENABLE_PARALLEL_CONSOLE_OUTPUT
	lpt_putc(c);
#endif
	cga_putc(c);
#if ENABLE_SERIAL_CONSOLE
	serial_putc_console(c);
#endif
}

// initialize the console devices
void
cons_init(void)
{
	cga_init();
	kbd_init();
	serial_init();
}


// `High'-level console I/O.  Used by readline and printf.

void
putchar(int c)
{
	cons_putc(c);
}

int
getchar(void)
{
	int c;

	while((c = cons_getc()) == -1);

	return c;
}

int
iscons(int fdnum)
{
	// used by readline
	return 1;
}
