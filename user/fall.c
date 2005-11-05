#include <inc/lib.h>
#include <inc/malloc.h>

/* from demo.c */
int rand(int nseed);

void fall(int argc, char * argv[])
{
	int rows = sys_vga_map_text(0xB8000) & ~1;
	short * b8 = (short *) 0xB8000;
	short * b8orig = malloc(rows * 80 * sizeof(short));
	short * b8copy = malloc(rows * 80 * sizeof(short));
	int column = -1;
	int i = -1, j = -1;
	
	if(!b8orig || !b8copy)
	{
		if(b8orig)
			free(b8orig);
		if(b8copy)
			free(b8copy);
		return;
	}
	
	memcpy(b8orig, b8, rows * 80 * sizeof(short));
	memcpy(b8copy, b8orig, rows * 80 * sizeof(short));
	
	while(getchar_nb() == -1)
	{
		/* if somebody changed the screen, bail out */
		if(memcmp(b8copy, b8, rows * 80 * sizeof(short)))
			break;
		
		/* are we currently making a character fall? */
		if(column != -1)
		{
			if(i == j)
				column = -1;
			else
			{
				short cell = b8copy[j * 80 + column];
				b8copy[j++ * 80 + column] = 0x0700 | ' ';
				b8copy[j * 80 + column] = cell;
				memcpy(b8, b8copy, rows * 80 * sizeof(short));
			}
		}
		/* else, only make a character fall every now and then */
		else if(!(rand(0) % 64))
		{
			column = rand(0) % 80;
			
			/* find where it will fall to */
			for(i = rows - 1; i != -1; i--)
			{
				unsigned char ch = b8copy[i * 80 + column] & 0xFF;
				if(!ch || ch == ' ' || ch == 255)
					break;
			}
			/* find where it will fall from */
			for(j = i; j != -1; j--)
			{
				unsigned char ch = b8copy[j * 80 + column] & 0xFF;
				if(ch && ch != ' ' && ch != 255)
					break;
			}
			
			/* if we didn't find one, reset */
			if(i == -1 || j == -1)
				column = -1;
		}
		
		sleepj(HZ / 20);
	}
	
	memcpy(b8, b8orig, rows * 80 * sizeof(short));
	
	free(b8orig);
	free(b8copy);
}
