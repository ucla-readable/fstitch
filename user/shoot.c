#include <inc/lib.h>
#include <inc/mouse.h>

/* from user/demo.c */
int rand(int nseed)
{
	static int seed = 0;
	if(nseed)
		seed = nseed;
	seed *= 214013;
	seed += 2531011;
	return (seed >> 16) & 0x7fff;
}

/* Colors:
 * 00 Black       0x000000
 * 01 Gray 1      0x606060
 * 02 Gray 2      0x808080
 * 03 Gray 3      0xA0A0A0
 * 04 White       0xFFFFFF
 * 05 Brown 1     0x351000
 * 06 Brown 2     0x631F00
 * 07 Brown 3     0x8E2A00
 * 08 Blue brown  0x374C64
 * 09 Blue 1      0x0A79C9
 * 0A Blue 2      0x0090FF
 * 0B Blue 3      0x63BBFF
 * 0C Green 1     0x059E00
 * 0D Green 2     0x08BF00
 * 0E Green 3     0x41FF3A
 * 0F Yellow      0xFFFF00
 * 10 Orange 1    0xFFA000
 * 11 Orange 2    0xFF8000
 * 12 Red         0xFF0000
 * 13 Purple      0xFF00FF
 * */

static uint8_t palette[768] = {0x00, 0x00, 0x00, 0x60, 0x60, 0x60,
                               0x80, 0x80, 0x80, 0xA0, 0xA0, 0xA0,
                               0xFF, 0xFF, 0xFF, 0x35, 0x10, 0x00,
                               0x63, 0x1F, 0x00, 0x8E, 0x2A, 0x00,
                               0x37, 0x4C, 0x64, 0x0A, 0x79, 0xC9,
                               0x00, 0x90, 0xFF, 0x63, 0xBB, 0xFF,
                               0x05, 0x9E, 0x00, 0x08, 0xBF, 0x00,
                               0x41, 0xFF, 0x3A, 0xFF, 0xFF, 0x00,
                               0xFF, 0xA0, 0x00, 0xFF, 0x80, 0x00,
                               0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF};

static const uint8_t ship[15][15] = {
	{0x02, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
	{0xFF, 0x02, 0x03, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x03, 0x03, 0xFF, 0xFF, 0xFF, 0xFF},
	{0xFF, 0x01, 0x0E, 0x02, 0x03, 0x0F, 0x00, 0x0F, 0x00, 0x0F, 0x00, 0x12, 0xFF, 0xFF, 0xFF},
	{0xFF, 0xFF, 0x02, 0x02, 0x02, 0x02, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
	{0xFF, 0xFF, 0x01, 0x02, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
	{0xFF, 0xFF, 0xFF, 0x02, 0x03, 0xFF, 0xFF, 0x03, 0x03, 0x03, 0x03, 0x03, 0xFF, 0xFF, 0xFF},
	{0xFF, 0x02, 0x02, 0x02, 0x02, 0x03, 0x03, 0x02, 0x02, 0x0A, 0x0B, 0x02, 0x03, 0x03, 0xFF},
	{0x01, 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x09, 0x0A, 0x0A, 0x0B, 0x02, 0x02, 0x03},
	{0xFF, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x09, 0x0A, 0x02, 0x02, 0x01, 0xFF},
	{0xFF, 0xFF, 0xFF, 0x02, 0x02, 0xFF, 0xFF, 0x01, 0x01, 0x01, 0x01, 0x01, 0xFF, 0xFF, 0xFF},
	{0xFF, 0xFF, 0x03, 0x02, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
	{0xFF, 0xFF, 0x02, 0x02, 0x02, 0x03, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
	{0xFF, 0x03, 0x0E, 0x02, 0x02, 0x0F, 0x00, 0x0F, 0x00, 0x0F, 0x00, 0x12, 0xFF, 0xFF, 0xFF},
	{0xFF, 0x02, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x01, 0xFF, 0xFF, 0xFF, 0xFF},
	{0x01, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
};

/* Layer 0: earth
 * Layer 1: water
 * Layer 2: ships
 * Layer 3: combine */
static uint8_t layers[4][64000];

static void combine_layers(void)
{
	int i, j;
	for(i = 0; i != 64000; i++)
	{
		uint8_t pixel = layers[0][i];
		for(j = 1; j != 3; j++)
			if(layers[j][i] != 0xFF)
				pixel = layers[j][i];
		layers[3][i] = pixel;
	}
}

static const uint8_t ground_map[6] = {5, 6, 6, 6, 6, 7};
static void shift_ground(void)
{
	int i, j;
	for(j = 0; j != 200; j++)
	{
		int offset = j * 320;
		for(i = 0; i != 319; i++)
			layers[0][offset + i] = layers[0][offset + i + 1];
		layers[0][offset + 319] = ground_map[rand(0) % 6];
	}
}

static const uint8_t water_map[5] = {9, 9, 9, 10, 11};
static int water_start[2], water_size[2] = {0}, water_duration[2];
static int water_sparkle = 1;
static void shift_water(void)
{
	int i, j;
	for(i = 0; i != 2; i++)
	{
		if(water_size[i] < 4)
			water_size[i] = 0;
		if(water_size[i])
		{
			if(++water_duration[i] < 5)
				water_size[i] += rand(0) % 4;
			else if(water_duration[i] < 15)
				water_size[i] += (rand(0) % 5) - 1;
			else if(water_duration[i] < 30)
				water_size[i] += (rand(0) % 3) - 1;
			else
				water_size[i] += (rand(0) % 5) - 3;
			water_start[i] += (rand(0) % 3) - 1;
		}
		else if(!(rand(0) % 128))
		{
			water_start[i] = rand(0) % 200;
			water_size[i] = 4;
			water_duration[i] = 0;
		}
	}
	for(j = 0; j != 200; j++)
	{
		int offset = j * 320;
		/* copy and sparkle old water */
		if(!(rand(0) % water_sparkle--))
		{
			water_sparkle = 15;
			for(i = 0; i != 319; i++)
			{
				uint8_t pixel = layers[1][offset + i + 1];
				if(pixel == 10)
					pixel = 11;
				else if(pixel == 11)
					pixel = 10;
				layers[1][offset + i] = pixel;
			}
		}
		else
			for(i = 0; i != 319; i++)
				layers[1][offset + i] = layers[1][offset + i + 1];
		/* form new water */
		for(i = 0; i != 2; i++)
		{
			if(!water_size[i])
				continue;
			if(water_start[i] == j || j == water_start[i] + water_size[i])
			{
				layers[1][offset + 319] = 8;
				break;
			}
			if(water_start[i] < j && j < water_start[i] + water_size[i])
			{
				layers[1][offset + 319] = water_map[rand(0) % 5];
				break;
			}
		}
		if(i == 2)
			layers[1][offset + 319] = 0xFF;
	}
}

static int ship_x = 10, ship_y = 95;
static int shot_x = -1, shot_y = -1;
static void draw_ships(int mouse)
{
	struct mouse_data data;
	int i, j, n = read_nb(mouse, &data, sizeof(struct mouse_data));
	while(n == sizeof(struct mouse_data))
	{
		ship_x += data.dx;
		ship_y -= data.dy;
		
		if(ship_x < 0)
			ship_x = 0;
		if(ship_y < 0)
			ship_y = 0;
		if(305 < ship_x)
			ship_x = 305;
		if(185 < ship_y)
			ship_y = 185;
		
		if(data.left && shot_x < 0)
		{
			shot_x = ship_x + 10;
			shot_y = ship_y + 2;
		}
		
		/* check for more mouse data */
		n = read_nb(mouse, &data, sizeof(struct mouse_data));
	}
	
	if(0 <= shot_x)
	{
		shot_x += 3;
		if(shot_x > 318)
			shot_x = -1;
	}
	
	memset(layers[2], 0xFF, 64000);
	if(0 <= shot_x)
		for(i = 0; i != 2; i++)
		{
			n = shot_y * 320 + shot_x + i * 3200;
			layers[2][n - 320] = 13;
			layers[2][n - 2] = 12;
			layers[2][n - 1] = 13;
			layers[2][n] = 14;
			layers[2][n + 1] = 13;
			layers[2][n + 320] = 13;
		}
	for(j = 0; j != 15; j++)
	{
		int offset = (ship_y + j) * 320;
		for(i = 0; i != 15; i++)
			if(ship[j][i] != 255)
				layers[2][offset + ship_x + i] = ship[j][i];
	}
}

static void play_shoot(int argc, char * argv[], uint8_t * vga, int mouse)
{
	int i;
	for(i = 0; i != 768; i++)
		palette[i] >>= 2;
	sys_vga_set_palette(palette, 0);
	
	rand(hwclock_time(NULL));
	
	/* draw bezel */
	for(i = 0; i != 320; i++)
	{
		vga[29760 + i] = 3;
		vga[30080 + i] = 1;
		vga[33600 + i] = 3;
		vga[33920 + i] = 1;
	}
	for(i = 0; i != 11; i++)
	{
		vga[30080 + i * 320] = 3;
		vga[30081 + i * 320] = 1;
		vga[30718 + i * 320] = 3;
		vga[30719 + i * 320] = 1;
	}
	
	for(i = 0; i != 320; i++)
	{
		int j;
		shift_ground();
		shift_water();
		/* progress bar */
		if(1 < i && i < 318)
			for(j = 0; j != 10; j++)
				vga[30400 + j * 320 + i] = 14;
	}
	
	while(getchar_nb() == -1)
	{
		shift_ground();
		shift_water();
		draw_ships(mouse);
		combine_layers();
		memcpy(vga, layers[3], 64000);
	}
}

void umain(int argc, char * argv[])
{
	int mouse = open_mouse();
	if(mouse < 0)
	{
		printf("%s: cannot open mouse\n", argv[0]);
		return;
	}
	if(0 <= sys_vga_set_mode_320(0xA0000))
	{
		play_shoot(argc, argv, (uint8_t *) 0xA0000, mouse);
		sys_vga_set_mode_text();
	}
	else
		printf("%s: cannot get video mode\n", argv[0]);
	close(mouse);
}
