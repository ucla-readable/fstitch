#include <inc/lib.h>

/* from demo.c */
int rand(int nseed);
extern short demo_buffer[3][50][80];

/* from wars.c */
extern unsigned char demo_font_map[256][8];
#define PIXEL(ch, x, y) ((demo_font_map[ch][y] >> (7 - (x))) & 1)

static void draw_char(short * buffer, unsigned char ch, int pos)
{
	int i, j;
	for(i = 0; i != 8; i++)
		for(j = 0; j != 8; j++)
			buffer[i * 80 + pos * 8 + j] = PIXEL(ch, j, i) ? 1 : 0;
}

void implode(int argc, char * argv[])
{
	int rows = sys_vga_map_text(0xB8000);
	short * b8 = (short *) 0xB8000;
	int i, t, points = 0;
	struct {
		int x, y, vx, vy;
	} * point = malloc(640 * sizeof(*point));
	
	rand(hwclock_time(NULL));
	printf("\n\n\n\n\n\n\n\n\n\n");
	
	/* [2] contains correct image */
	for(i = 0; argv[1][i] && i != 10; i++)
		draw_char((short *) demo_buffer[2], argv[1][i], i);
	/* [1] contains the blank screen */
	memcpy(demo_buffer[1], b8, 80 * rows * 2);
	for(i = 0; i != 640; i++)
	{
		demo_buffer[1][rows - 10 + i / 80][i % 80] = 0x0700 | ' ';
		if(demo_buffer[2][i / 80][i % 80])
		{
			point[points].x = i % 80;
			point[points].y = rows - 10 + i / 80;
			do {
				point[points].vx = (rand(0) % 5) - 2;
				point[points].vy = (rand(0) % 5) - 2;
			} while(!point[points].vx && !point[points].vy);
			points++;
		}
	}
	/* rewind time */
	for(t = 0; t != 30; t++)
		for(i = 0; i != points; i++)
		{
			point[i].x -= point[i].vx;
			point[i].y -= point[i].vy;
		}
	/* and play it forward again */
	for(t = 0; t != 30; t++)
	{
		/* [0] contains the new screen */
		memcpy(demo_buffer[0], demo_buffer[1], 80 * rows * 2);
		for(i = 0; i != points; i++)
		{
			point[i].x += point[i].vx;
			point[i].y += point[i].vy;
			if(point[i].x < 0 || point[i].y < 0)
				continue;
			if(point[i].x >= 80 || point[i].y >= rows)
				continue;
			demo_buffer[0][point[i].y][point[i].x] = 0x09B1;
		}
		memcpy(b8, demo_buffer[0], 80 * rows * 2);
		jsleep(HZ / 15);
	}
	jsleep(HZ / 4);
	/* shimmer across */
	for(t = 0; t != 100; t++)
	{
		int j;
		for(j = 0; j != 8; j++)
			for(i = 0; i < t - j && i < 80; i++)
				if(demo_buffer[2][j][i])
				{
					if(i + 4 > t - j || (i + 8 > t - j && i + 6 <= t - j))
						demo_buffer[0][rows - 10 + j][i] = 0x0FDB;
					else
						demo_buffer[0][rows - 10 + j][i] = 0x09DB;
				}
		memcpy(b8, demo_buffer[0], 80 * rows * 2);
		jsleep(HZ / 100);
	}
}

static const int delay[8] = {10, 9, 7, 4, 0, 4, 7, 9};

void bullet(int argc, char * argv[])
{
	int rows = sys_vga_map_text(0xB8000);
	short * b8 = (short *) 0xB8000;
	int i, j, bullet = 0;
	
	printf("\n\n\n\n\n\n\n\n\n\n");
	b8 = &b8[80 * (rows - 10)];
	
	for(i = 0; argv[1][i] && i != 10; i++)
		draw_char((short *) demo_buffer[1], argv[1][i], i);
	
	for(i = 0; i != 640; i++)
		demo_buffer[0][i / 80][i % 80] = 0x0700 | ' ';
	for(bullet = 0; bullet != 100; bullet++)
	{
		for(j = 0; j != 8; j++)
			for(i = 0; i < bullet - delay[j] && i < 80; i++)
				if(demo_buffer[1][j][i])
				{
					if(i + 8 > bullet - delay[j])
						demo_buffer[0][j][i] = 0x0FB2;
					else
						demo_buffer[0][j][i] = 0x01B0;
				}
		memcpy(b8, demo_buffer[0], 1280);
		jsleep(HZ / 50);
	}
	for(bullet = 0; bullet != 100; bullet++)
	{
		for(j = 0; j != 8; j++)
			for(i = 0; i < bullet - j && i < 80; i++)
				if(demo_buffer[1][j][i])
				{
					if(i + 4 > bullet - j || (i + 8 > bullet - j && i + 6 <= bullet - j))
						demo_buffer[0][j][i] = 0x0FDB;
					else
						demo_buffer[0][j][i] = 0x09B1;
				}
		memcpy(b8, demo_buffer[0], 1280);
		jsleep(HZ / 100);
	}
	for(j = 0; j != 8; j++)
		for(i = 0; i != 80; i++)
			if(demo_buffer[1][j][i])
				demo_buffer[0][j][i] = 0x0FDB;
	memcpy(b8, demo_buffer[0], 1280);
	jsleep(HZ / 50);
	for(j = 0; j != 8; j++)
		for(i = 0; i != 80; i++)
			if(demo_buffer[1][j][i])
				demo_buffer[0][j][i] = 0x09DB;
	memcpy(b8, demo_buffer[0], 1280);
}
