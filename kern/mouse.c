#include <kern/mouse.h>
#include <kern/picirq.h>
#include <kern/irq.h>
#include <kern/console.h>

#include <inc/kbdreg.h>

/*
 * # Check for PS/2 pointing device
 * movw    %cs, %ax              # aka SETUPSEG
 * subw    $DELTA_INITSEG, %ax   # aka INITSEG
 * movw    %ax, %ds
 * movw    $0, (0x1ff)           # default is no pointing device
 * int     $0x11                 # int 0x11: equipment list
 * testb   $0x04, %al            # check if mouse installed
 * jz      no_psmouse
 * 
 * movw    $0xAA, (0x1ff)        # device present
 * */

#define MOUSE_BUFFER_SIZE 1024
static uint8_t mouse_buffer[MOUSE_BUFFER_SIZE];
static uint16_t mb_rpos = 0, mb_wpos = 0;

int mouse_detect(void)
{
	return 0;
}

int mouse_read(uint8_t * buffer, int size)
{
	int written = 0;
	
	if(mb_rpos == mb_wpos)
		return -1;
	
	while(written < size && mb_rpos != mb_wpos)
	{
		buffer[written++] = mouse_buffer[mb_rpos];
		mb_rpos = (mb_rpos + 1) % MOUSE_BUFFER_SIZE;
	}
	
	return written;
}

int mouse_command(uint8_t command)
{
	while(inb(KBSTATP) & KBS_IBF);
	outb(KBCMDP, KBC_AUXWRITE);
	
	while(inb(KBSTATP) & KBS_IBF);
	outb(KBDATAP, command);
	
	return 0;
}

void mouse_intr(int irq)
{
	uint8_t status = inb(KBSTATP);
	while(status & KBS_DIB)
	{
		if(status & KBS_AUXD)
		{
			uint8_t data = inb(KBDATAP);
			uint16_t nwp = (mb_wpos + 1) % MOUSE_BUFFER_SIZE;
			
			if(nwp != mb_rpos)
			{
				mouse_buffer[mb_wpos] = data;
				mb_wpos = nwp;
			}
			else
				printf("mouse buffer full!\n");
		}
		else
			kbd_intr(irq);
		status = inb(KBSTATP);
	}
}

int mouse_init(void)
{
	/* just assume the mouse is there and initialize it... */
	
	while(inb(KBSTATP) & KBS_IBF);
	outb(KBCMDP, KBC_AUXENABLE);
	
	while(inb(KBSTATP) & KBS_IBF);
	outb(KBCMDP, KBC_WRITEMODE);
	
	while(inb(KBSTATP) & KBS_IBF);
	outb(KBDATAP, CMDBYTE);
	
	request_irq(MOUSE_IRQ, mouse_intr);
	irq_setmask_8259A(irq_mask_8259A & ~(1 << MOUSE_IRQ));
	
	return 0;
}
