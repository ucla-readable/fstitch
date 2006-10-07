#include <inc/lib.h>
#include <inc/math.h>

/* from demo.c */
int rand(int nseed);
extern uint8_t demo_buffer[1][64000];

typedef struct {
	int cycle;
	uint8_t buffer[2][128 * 20];
} fire_t;

static void polarwarp_fire(fire_t * fire, uint8_t * screen, int x, int y)
{
	int i, j;
	for(i = 0; i != 128; i++)
	{
		double a = M_PI * i / 64.0;
		double sa = sin(a), ca = cos(a);
		for(j = 0; j != 20; j++)
		{
			int px = (int) (j * ca) + x;
			int py = (int) (j * sa) + y;
			uint8_t * orig = &screen[py * 320 + px];
			uint8_t replace = fire->buffer[1][j * 128 + i];
			if(replace > *orig)
				*orig = replace;
		}
	}
}

static void advance_fire(fire_t * fire)
{
	int i, j;
	
	memcpy(&fire->buffer[0][128 * 2], fire->buffer[1], 128 * 18);
	if(fire->cycle < 4)
		for(i = 0; i != 128 * 2; i++)
			fire->buffer[0][i] = rand(0);
	else
		memset(fire->buffer[0], 0, 128 * 2);
	if(++fire->cycle == 16)
		fire->cycle = 0;
	
	for(i = 0; i != 128; i++)
	{
		int n, x[3] = {(i + 127) % 128, i, (i + 1) % 128};
		for(j = 0; j != 20; j++)
		{
			uint16_t total = 0;
			if(j)
				for(n = 0; n != 3; n++)
					total += fire->buffer[0][(j - 1) * 128 + x[n]];
			for(n = 0; n != 3; n++)
				total += fire->buffer[0][j * 128 + x[n]];
			if(j != 19)
				for(n = 0; n != 3; n++)
					total += fire->buffer[0][(j + 1) * 128 + x[n]];
			total = total * 2 / 17;
			fire->buffer[1][j * 128 + i] = (total > 15) ? total - 16 : 0;
		}
	}
}

static fire_t fires[8];

void explode(int argc, char * argv[])
{
	int i;
	uint8_t palette[768];
	
	/* this portion of the palette will become transparent */
	for(i = 0; i != 64; i++)
	{
		palette[3 * i] = i;
		palette[3 * i + 1] = 0;
		palette[3 * i + 2] = 0;
	}
	/* the rest of the palette is opaque */
	for(; i != 128; i++)
	{
		palette[3 * i] = 63;
		palette[3 * i + 1] = i;
		palette[3 * i + 2] = 0;
	}
	for(; i != 192; i++)
	{
		palette[3 * i] = 63;
		palette[3 * i + 1] = 63;
		palette[3 * i + 2] = i;
	}
	/* this portion is not really used in practice... */
	for(; i != 256; i++)
	{
		palette[3 * i] = 63;
		palette[3 * i + 1] = 63;
		palette[3 * i + 2] = 63;
	}
	
	for(i = 0; i != 8; i++)
	{
		fires[i].cycle = 2 * i;
		memset(fires[i].buffer[0], 0, 128 * 20);
		memset(fires[i].buffer[1], 0, 128 * 20);
	}
	
	if(sys_vga_set_mode_320(0xA0000, 0) < 0)
		exit(1);
	sys_vga_set_palette(palette, 0);
	
	while(getchar_nb() == -1)
	{
		memset(demo_buffer[0], 0, 64000);
		for(i = 0; i != 8; i++)
		{
			advance_fire(&fires[i]);
			polarwarp_fire(&fires[i], demo_buffer[0], 20 + i * 40, 100);
		}
		memcpy((void *) 0xA0000, demo_buffer[0], 64000);
		
		sys_yield();
	}
	
	sys_vga_set_mode_text(0);
}
