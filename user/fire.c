#include <inc/lib.h>
#include <inc/mouse.h>

/* from demo.c */
int rand(int nseed);
extern uint8_t demo_buffer[2][64000];

void fire(int argc, char * argv[])
{
	int i, j, mouse_fd, button = 0;
	uint8_t palette[768];
	
	mouse_fd = open_mouse();
	
	for(i = 0; i != 64; i++)
	{
		palette[3 * i] = i;
		palette[3 * i + 1] = 0;
		palette[3 * i + 2] = 0;
	}
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
	for(; i != 256; i++)
	{
		palette[3 * i] = 63;
		palette[3 * i + 1] = 63;
		palette[3 * i + 2] = 63;
	}
	
	memset(demo_buffer[0], 0, 64000);
	memset(demo_buffer[1], 0, 64000);
	
	if(sys_vga_set_mode_320(0xA0000, 0) < 0)
		exit(1);
	sys_vga_set_palette(palette, 0);
	
	while(getchar_nb() == -1)
	{
		if(mouse_fd > 0)
		{
			struct mouse_data data;
			int n = read_nb(mouse_fd, &data, sizeof(struct mouse_data));
			if(n == sizeof(struct mouse_data))
				button = data.left + data.middle + data.right;
			else if(0 <= n)
			{
				close(mouse_fd);
				mouse_fd = -1;
			}
		}
		
		memcpy(demo_buffer[0], &demo_buffer[1][640], 64000 - 640);
		if(button)
			for(i = 0; i != 640; i++)
				demo_buffer[0][64000 - 640 + i] = rand(0) | 128;
		else
			for(i = 0; i != 640; i++)
				demo_buffer[0][64000 - 640 + i] = rand(0);
		
		for(i = 0; i != 320; i++)
		{
			int n, x[3] = {(i + 319) % 320, i, (i + 1) % 320};
			/* calculate only the bottom 1/4 of the buffer */
			for(j = 150; j != 200; j++)
			{
				uint16_t total = 0;
				if(j)
					for(n = 0; n != 3; n++)
						total += demo_buffer[0][(j - 1) * 320 + x[n]];
				for(n = 0; n != 3; n++)
					total += demo_buffer[0][j * 320 + x[n]];
				if(j != 199)
					for(n = 0; n != 3; n++)
						total += demo_buffer[0][(j + 1) * 320 + x[n]];
				total = total * 2 / 17;
				demo_buffer[1][j * 320 + i] = (total > 15) ? total - 16 : 0;
			}
		}
		
		memcpy((void *) 0xA0000, demo_buffer[1], 64000);
		
		sys_yield();
	}
	
	sys_vga_set_mode_text(0);
}
