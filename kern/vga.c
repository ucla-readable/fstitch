#include <inc/x86.h>
#include <inc/string.h>
#include <inc/error.h>

#include <kern/vga.h>
#include <kern/pmap.h>
#include <kern/kclock.h>

struct vga_pio {
	uint16_t port;
	uint8_t value;
	uint8_t read;
};

/* This sequence will set 80x25 text mode */
const struct vga_pio pio_text[] = {
	{0x3da, 0x00, 1}, {0x3c0, 0x00, 0}, {0x3c0, 0x00, 0}, {0x3c0, 0x01, 0},
	{0x3c0, 0x01, 0}, {0x3c0, 0x02, 0}, {0x3c0, 0x02, 0}, {0x3c0, 0x03, 0},
	{0x3c0, 0x03, 0}, {0x3c0, 0x04, 0}, {0x3c0, 0x04, 0}, {0x3c0, 0x05, 0},
	{0x3c0, 0x05, 0}, {0x3c0, 0x06, 0}, {0x3c0, 0x14, 0}, {0x3c0, 0x07, 0},
	{0x3c0, 0x07, 0}, {0x3c0, 0x08, 0}, {0x3c0, 0x38, 0}, {0x3c0, 0x09, 0},
	{0x3c0, 0x39, 0}, {0x3c0, 0x0a, 0}, {0x3c0, 0x3a, 0}, {0x3c0, 0x0b, 0},
	{0x3c0, 0x3b, 0}, {0x3c0, 0x0c, 0}, {0x3c0, 0x3c, 0}, {0x3c0, 0x0d, 0},
	{0x3c0, 0x3d, 0}, {0x3c0, 0x0e, 0}, {0x3c0, 0x3e, 0}, {0x3c0, 0x0f, 0},
	{0x3c0, 0x3f, 0}, {0x3c0, 0x10, 0}, {0x3c0, 0x0c, 0}, {0x3c0, 0x11, 0},
	{0x3c0, 0x00, 0}, {0x3c0, 0x12, 0}, {0x3c0, 0x0f, 0}, {0x3c0, 0x13, 0},
	{0x3c0, 0x08, 0}, {0x3c0, 0x14, 0}, {0x3c0, 0x00, 0}, {0x3c4, 0x00, 0},
	{0x3c5, 0x03, 0}, {0x3c4, 0x01, 0}, {0x3c5, 0x00, 0}, {0x3c4, 0x02, 0},
	{0x3c5, 0x03, 0}, {0x3c4, 0x03, 0}, {0x3c5, 0x00, 0}, {0x3c4, 0x04, 0},
	{0x3c5, 0x02, 0}, {0x3ce, 0x00, 0}, {0x3cf, 0x00, 0}, {0x3ce, 0x01, 0},
	{0x3cf, 0x00, 0}, {0x3ce, 0x02, 0}, {0x3cf, 0x00, 0}, {0x3ce, 0x03, 0},
	{0x3cf, 0x00, 0}, {0x3ce, 0x04, 0}, {0x3cf, 0x00, 0}, {0x3ce, 0x05, 0},
	{0x3cf, 0x10, 0}, {0x3ce, 0x06, 0}, {0x3cf, 0x0e, 0}, {0x3ce, 0x07, 0},
	{0x3cf, 0x0f, 0}, {0x3ce, 0x08, 0}, {0x3cf, 0xff, 0}, {0x3d4, 0x00, 0},
	{0x3d5, 0x5f, 0}, {0x3d4, 0x01, 0}, {0x3d5, 0x4f, 0}, {0x3d4, 0x02, 0},
	{0x3d5, 0x50, 0}, {0x3d4, 0x03, 0}, {0x3d5, 0x82, 0}, {0x3d4, 0x04, 0},
	{0x3d5, 0x55, 0}, {0x3d4, 0x05, 0}, {0x3d5, 0x81, 0}, {0x3d4, 0x06, 0},
	{0x3d5, 0xbf, 0}, {0x3d4, 0x07, 0}, {0x3d5, 0x1f, 0}, {0x3d4, 0x08, 0},
	{0x3d5, 0x00, 0}, {0x3d4, 0x09, 0}, {0x3d5, 0x4f, 0}, {0x3d4, 0x0a, 0},
	{0x3d5, 0x0d, 0}, {0x3d4, 0x0b, 0}, {0x3d5, 0x0e, 0}, {0x3d4, 0x0c, 0},
	{0x3d5, 0x00, 0}, {0x3d4, 0x0d, 0}, {0x3d5, 0x00, 0}, {0x3d4, 0x10, 0},
	{0x3d5, 0x9c, 0}, {0x3d4, 0x11, 0}, {0x3d5, 0x8e, 0}, {0x3d4, 0x12, 0},
	{0x3d5, 0x8f, 0}, {0x3d4, 0x13, 0}, {0x3d5, 0x28, 0}, {0x3d4, 0x14, 0},
	{0x3d5, 0x1f, 0}, {0x3d4, 0x15, 0}, {0x3d5, 0x96, 0}, {0x3d4, 0x16, 0},
	{0x3d5, 0xb9, 0}, {0x3d4, 0x17, 0}, {0x3d5, 0xa3, 0}, {0x3d4, 0x18, 0},
	{0x3d5, 0xff, 0}, {0x3c2, 0x67, 0}, {0x3c0, 0x20, 0}, {0x3da, 0x00, 1},
	{0x3d4, 0x0a, 0}, {0x3d5, 0x0e, 0}, {0x3d4, 0x0b, 0}, {0x3d5, 0x0f, 0},
	{0x3d4, 0x0c, 0}, {0x3d5, 0x00, 0}, {0x3d4, 0x0d, 0}, {0x3d5, 0x00, 0},
	{0x3c4, 0x03, 0}, {0x3c5, 0x00, 0}, {0x3c4, 0x03, 0}, {0x3c5, 0x00, 0}
};

/* This sequence will set 320x200x256 graphics mode */
const struct vga_pio pio_320[] = {
	{0x3da, 0x00, 1}, {0x3c0, 0x00, 0}, {0x3c0, 0x00, 0}, {0x3c0, 0x01, 0},
	{0x3c0, 0x01, 0}, {0x3c0, 0x02, 0}, {0x3c0, 0x02, 0}, {0x3c0, 0x03, 0},
	{0x3c0, 0x03, 0}, {0x3c0, 0x04, 0}, {0x3c0, 0x04, 0}, {0x3c0, 0x05, 0},
	{0x3c0, 0x05, 0}, {0x3c0, 0x06, 0}, {0x3c0, 0x06, 0}, {0x3c0, 0x07, 0},
	{0x3c0, 0x07, 0}, {0x3c0, 0x08, 0}, {0x3c0, 0x08, 0}, {0x3c0, 0x09, 0},
	{0x3c0, 0x09, 0}, {0x3c0, 0x0a, 0}, {0x3c0, 0x0a, 0}, {0x3c0, 0x0b, 0},
	{0x3c0, 0x0b, 0}, {0x3c0, 0x0c, 0}, {0x3c0, 0x0c, 0}, {0x3c0, 0x0d, 0},
	{0x3c0, 0x0d, 0}, {0x3c0, 0x0e, 0}, {0x3c0, 0x0e, 0}, {0x3c0, 0x0f, 0},
	{0x3c0, 0x0f, 0}, {0x3c0, 0x10, 0}, {0x3c0, 0x41, 0}, {0x3c0, 0x11, 0},
	{0x3c0, 0x00, 0}, {0x3c0, 0x12, 0}, {0x3c0, 0x0f, 0}, {0x3c0, 0x13, 0},
	{0x3c0, 0x00, 0}, {0x3c0, 0x14, 0}, {0x3c0, 0x00, 0}, {0x3c4, 0x00, 0},
	{0x3c5, 0x03, 0}, {0x3c4, 0x01, 0}, {0x3c5, 0x01, 0}, {0x3c4, 0x02, 0},
	{0x3c5, 0x0f, 0}, {0x3c4, 0x03, 0}, {0x3c5, 0x00, 0}, {0x3c4, 0x04, 0},
	{0x3c5, 0x0e, 0}, {0x3ce, 0x00, 0}, {0x3cf, 0x00, 0}, {0x3ce, 0x01, 0},
	{0x3cf, 0x00, 0}, {0x3ce, 0x02, 0}, {0x3cf, 0x00, 0}, {0x3ce, 0x03, 0},
	{0x3cf, 0x00, 0}, {0x3ce, 0x04, 0}, {0x3cf, 0x00, 0}, {0x3ce, 0x05, 0},
	{0x3cf, 0x40, 0}, {0x3ce, 0x06, 0}, {0x3cf, 0x05, 0}, {0x3ce, 0x07, 0},
	{0x3cf, 0x0f, 0}, {0x3ce, 0x08, 0}, {0x3cf, 0xff, 0}, {0x3d4, 0x00, 0},
	{0x3d5, 0x5f, 0}, {0x3d4, 0x01, 0}, {0x3d5, 0x4f, 0}, {0x3d4, 0x02, 0},
	{0x3d5, 0x50, 0}, {0x3d4, 0x03, 0}, {0x3d5, 0x82, 0}, {0x3d4, 0x04, 0},
	{0x3d5, 0x54, 0}, {0x3d4, 0x05, 0}, {0x3d5, 0x80, 0}, {0x3d4, 0x06, 0},
	{0x3d5, 0xbf, 0}, {0x3d4, 0x07, 0}, {0x3d5, 0x1f, 0}, {0x3d4, 0x08, 0},
	{0x3d5, 0x00, 0}, {0x3d4, 0x09, 0}, {0x3d5, 0x41, 0}, {0x3d4, 0x0a, 0},
	{0x3d5, 0x00, 0}, {0x3d4, 0x0b, 0}, {0x3d5, 0x00, 0}, {0x3d4, 0x0c, 0},
	{0x3d5, 0x00, 0}, {0x3d4, 0x0d, 0}, {0x3d5, 0x00, 0}, {0x3d4, 0x10, 0},
	{0x3d5, 0x9c, 0}, {0x3d4, 0x11, 0}, {0x3d5, 0x8e, 0}, {0x3d4, 0x12, 0},
	{0x3d5, 0x8f, 0}, {0x3d4, 0x13, 0}, {0x3d5, 0x28, 0}, {0x3d4, 0x14, 0},
	{0x3d5, 0x40, 0}, {0x3d4, 0x15, 0}, {0x3d5, 0x96, 0}, {0x3d4, 0x16, 0},
	{0x3d5, 0xb9, 0}, {0x3d4, 0x17, 0}, {0x3d5, 0xa3, 0}, {0x3d4, 0x18, 0},
	{0x3d5, 0xff, 0}, {0x3c2, 0x63, 0}, {0x3c0, 0x20, 0}, {0x3da, 0x00, 1},
	{0x3c4, 0x02, 0}, {0x3c5, 0x0f, 1}, {0x3c5, 0x0f, 0}, {0x3c5, 0x0f, 0},
	{0x3d4, 0x0c, 0}, {0x3d5, 0x00, 0}, {0x3d4, 0x0d, 0}, {0x3d5, 0x00, 0}
};

void vga_save_palette(uint8_t * buffer)
{
	int i;
	outb(0x3c7, 0x00);
	for(i = 0; i != 768; i++)
		buffer[i] = inb(0x3c9);
}

void vga_set_palette(const uint8_t * buffer, uint8_t dim)
{
	int i;
	outb(0x3c8, 0x00);
	for(i = 0; i != 768; i++)
		outb(0x3c9, (dim > buffer[i]) ? 0 : buffer[i] - dim);
}

/* For some reason save_font() and set_font() need to be in graphics mode to work... */
static void vga_save_font(uint8_t * buffer)
{
	uint8_t i, idx;
	outb(0x3ce, 0x04);
	idx = inb(0x3cf);
	for(i = 0; i != 4; i++)
	{
		outb(0x3ce, 0x04);
		outb(0x3cf, i);
		memcpy(buffer + i * VGA_MEM_SIZE, VGA_MEM, VGA_MEM_SIZE);
	}
	outb(0x3ce, 0x04);
	outb(0x3cf, idx);
}

static void vga_set_font(const uint8_t * buffer)
{
	uint8_t i, mask;
	outb(0x3c4, 0x02);
	mask = inb(0x3c5);
	for(i = 0; i != 4; i++)
	{
		outb(0x3c4, 0x02);
		outb(0x3c5, 1 << i);
		memcpy(VGA_MEM, buffer + i * VGA_MEM_SIZE, VGA_MEM_SIZE);
	}
	outb(0x3c4, 0x02);
	outb(0x3c5, mask);
}

static void vga_set_mode(const struct vga_pio * pio, int size)
{
	int i;
	for(i = 0; i != size; i++)
	{
		if(pio[i].read)
			inb(pio[i].port);
		else
			outb(pio[i].port, pio[i].value);
	}
}

static uint8_t vga_palette[768], vga_cell;
static uint8_t vga_cur_start, vga_cur_end;
static uint8_t vga_font[4 * VGA_MEM_SIZE];
static int graphics = 0;

int vga_set_mode_320(int fade)
{
	int i;
	
	if(graphics)
		return -E_BUSY;
	graphics = 1;
	
	/* save 25/50 line mode */
	outb(0x3d4, 0x9);
	vga_cell = inb(0x3d5);
	outb(0x3d4, 0xA);
	vga_cur_start = inb(0x3d5);
	outb(0x3d4, 0xB);
	vga_cur_end = inb(0x3d5);
	
	/* save and dim palette */
	vga_save_palette(vga_palette);
	if(fade)
		for(i = 1; i != 64; i++)
		{
			vga_set_palette(vga_palette, i);
			kclock_delay(3);
		}
	else
		vga_set_palette(vga_palette, 63);
	
	/* change to graphics mode */
	vga_set_mode(pio_320, sizeof(pio_320) / sizeof(pio_320[0]));
	vga_save_font(vga_font);
	
	/* clear screen */
	memset(VGA_MEM, 0, 64000);
	
	/* set new palette */
	outb(0x3c8, 0x00);
	for(i = 0; i != 256; i++)
	{
		outb(0x3c9, i >> 2);
		outb(0x3c9, i >> 2);
		outb(0x3c9, i >> 2);
	}
	
	return 0;
}

int vga_set_mode_text(int fade)
{
	int i;
	
	if(!graphics)
		return -E_BUSY;
	graphics = 0;
	
	/* clear palette */
	vga_set_palette(vga_palette, 63);
	
	/* restore screen */
	vga_set_font(vga_font);
	vga_set_mode(pio_text, sizeof(pio_text) / sizeof(pio_text[0]));
	
	/* restore 25/50 line mode */
	outb(0x3d4, 0x9);
	outb(0x3d5, vga_cell);
	outb(0x3d4, 0xA);
	outb(0x3d5, vga_cur_start);
	outb(0x3d4, 0xB);
	outb(0x3d5, vga_cur_end);
	
	/* restore palette */
	if(fade)
		for(i = 62; i != -1; i--)
		{
			vga_set_palette(vga_palette, i);
			kclock_delay(3);
		}
	else
		vga_set_palette(vga_palette, 0);
	
	return 0;
}