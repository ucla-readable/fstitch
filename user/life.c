#include <inc/lib.h>
#include <inc/malloc.h>

/* from demo.c */
int rand(int nseed);

void life(int argc, char * argv[])
{
	const int next_age_map[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 3, 0, 4, 0, 2, 0};
	const int color_map[5] = {0, 14, 10, 12, 9};
	
	int x = 0, y;
	int rows = sys_vga_map_text(0xB8000);
	
	char * b8 = (char *) 0xB8000;
	char * next_gen = malloc(80 * rows);
	
	if(argc > 1)
		rand(strtol(argv[1], NULL, 0));
	
	while(x != 160 * rows)
	{
		b8[x++] = 1;
		b8[x++] = color_map[rand(0) & 1];
	}
	
	while(getchar_nb() == -1)
	{
		for(y = 0; y != rows; y++)
			for(x = 0; x != 80; x++)
			{
				int n = 0, i, j;
				char * cell = &next_gen[y * 80 + x];
				
				const int dx[3] = {(x + 79) % 80, x, (x + 1) % 80};
				const int dy[3] = {(y + rows - 1) % rows, y, (y + 1) % rows};
				
				for(j = 0; j != 3; j++)
					for(i = 0; i != 3; i++)
					{
						if(i == 1 && j == 1)
							continue;
						if(b8[(dy[j] * 80 + dx[i]) * 2 + 1] != color_map[0])
							n++;
					}
				
				*cell = next_age_map[(int) b8[(y * 80 + x) * 2 + 1]];
				if(*cell)
				{
					if(n == 2 || n == 3)
						*cell = color_map[(int) *cell];
					else
						*cell = color_map[0];
				}
				else
					*cell = color_map[n == 3];
			}
		for(x = 0, y = 1; x != 80 * rows; x++, y += 2)
			b8[y] = next_gen[x];
	}
	
	free(next_gen);
}
