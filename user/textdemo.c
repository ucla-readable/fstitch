#include <inc/lib.h>
#include <inc/malloc.h>

/* from demo.c */
int rand(int nseed);

void swirl(int argc, char * argv[])
{
	int rows = sys_vga_map_text(0xB8000) & ~1;
	short * b8 = (short *) 0xB8000;
	short * b8buf = malloc(rows * 160 * sizeof(short));
	
	while(getchar_nb() == -1)
	{
		int i, j;
		for(i = 0; i != rows / 2; i++)
		{
			for(j = i; j != 79 - i; j++)
			{
				int offset = 80 * i + j;
				b8buf[offset + 1] = b8[offset];
				offset = rows * 80 - 1 - offset;
				b8buf[offset - 1] = b8[offset];
			}
			for(j = i; j != rows - 1 - i; j++)
			{
				int offset = 80 * j + i;
				b8buf[offset] = b8[offset + 80];
				offset = rows * 80 - 1 - offset;
				b8buf[offset] = b8[offset - 80];
			}
		}
		memcpy(b8, b8buf, rows * 160);
	}
	
	free(b8buf);
}

void data(int argc, char * argv[])
{
	short * b8 = (short *) 0xB8000;
	int i, rows = sys_vga_map_text(0xB8000);
	
	while(getchar_nb() == -1)
		for(i = 0; i != 80 * rows; i++)
			b8[i] = rand(0);
}
