#include <inc/lib.h>

/* from demo.c */
int rand(int nseed);
extern uint8_t demo_buffer[2][64000];

/* from wars.c */
extern unsigned char demo_font_map[256][8];
#define PIXEL(ch, x, y) ((demo_font_map[ch][y] >> (7 - (x))) & 1)

/* green can vary from 0 (no green) to 128 (full green) */
static void set_palette(int green)
{
	int i;
	int gray = 128 - green;
	for(i = 0; i != 128; i++)
	{
		uint8_t pixel = i >> 1;
		uint8_t fade = (i * gray) >> 8;
		demo_buffer[1][0 + 3 * i] = pixel;
		demo_buffer[1][1 + 3 * i] = pixel;
		demo_buffer[1][2 + 3 * i] = pixel;
		demo_buffer[1][384 + 3 * i] = fade;
		demo_buffer[1][385 + 3 * i] = (i * gray + 126 * green) >> 8;
		demo_buffer[1][386 + 3 * i] = fade;
	}
	sys_vga_set_palette(demo_buffer[1], 0);
}

/* rank 1 is rightmost */
static void draw_digit(int digit, int rank)
{
	int i, j;
	digit += '0';
	for(i = 0; i != 16; i++)
		for(j = 0; j != 16; j++)
			if(PIXEL(digit, j / 2, i / 2))
				demo_buffer[0][(i + 3) * 320 - rank * 18 + j] |= 128;
}

void tv(int argc, char * argv[])
{
	int c, green = 0;
	int channel = 3;
	int number = 0;
	
	sys_vga_set_mode_320(0xA0000);
	set_palette(0);
	
	c = getchar_nb();
	while(c != 'q' && c != 27)
	{
		if(c > 0)
		{
			if(c == '+' || c == KEYCODE_UP)
			{
				if(++channel == 100)
					channel = 1;
				number = 0;
			}
			else if(c == '-' || c == KEYCODE_DOWN)
			{
				if(!--channel)
					channel = 99;
				number = 0;
			}
			else if('0' <= c && c <= '9')
			{
				if(64 <= green && number)
					channel = 10 * (channel % 10) + c - '0';
				else
					channel = c - '0';
				number = 1;
			}
			green = 129;
		}
		
		/* make the snow */
		for(c = 0; c != 64000; c++)
			demo_buffer[0][c] = rand(0) & 127;
		
		if(green)
		{
			draw_digit(channel / 10, 2);
			draw_digit(channel % 10, 1);
			/* nice parabolic brightness curve */
			green = 129 - green;
			set_palette(128 - ((green * green) >> 7));
			green = 128 - green;
		}
		
		memcpy((void *) 0xA0000, demo_buffer[0], 64000);
		
		sys_yield();
		c = getchar_nb();
	}
	
	sys_vga_set_mode_text();
}
