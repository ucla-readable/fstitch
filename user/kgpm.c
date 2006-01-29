#include <inc/lib.h>
#include <inc/mouse.h>

#define CONSOLE 0xB8000

static uint16_t * console = (uint16_t *) CONSOLE;

static inline void cput(int row, int col, uint16_t c, int rows, int cols)
{
        assert(row >= 0 && row < rows);
        assert(col >= 0 && col < cols);
        console[row * 80 + col] = c;
}

static inline uint16_t cget(int row, int col, int rows, int cols)
{
        assert(row >= 0 && row < rows);
        assert(col >= 0 && col < cols);
        return console[row * 80 + col];
}

static inline void up_down(int * x0, int x1, const char * name)
{
	if(*x0 == x1)
		return;
	/* have some fun! start the fall demo on mouse up */
	if(!x1)
		spawnl("/demo", "/demo", "fall", NULL);
	*x0 = x1;
}

static inline void move(int * x, int dx, int upper_bound)
{
	*x += dx;
	if(*x < 0)
		*x = 0;
	else if(*x >= upper_bound)
		*x = upper_bound - 1;
}

void umain(void)
{
	int fd, cols = 80, rows, x = 0, y = 0;
	int c = 0x7000, oldc = -1, left = 0, middle = 0, right = 0;
	
	if(fork() != 0)
		return;
	
	fd = open_mouse();
	if(fd < 0)
	{
		printf("kgpm: %e\n", fd);
		return;
	}
	
	rows = sys_vga_map_text(CONSOLE);
	if(rows < 0)
	{
		printf("error: unable to map console memory\n");
		return;
	}
	
	for(;;)
	{
		struct mouse_data data;
		int i, j, num_read = 0;
		while(num_read != sizeof(struct mouse_data))
		{
			int n = read(fd, ((uint8_t *) &data) + num_read, sizeof(struct mouse_data) - num_read);
			if(n < 0)
				sys_yield();
			else if(!n)
				break;
			else
				num_read += n;
		}
		
		if(num_read == sizeof(struct mouse_data))
		{
			up_down(&left, data.left, "left");
			up_down(&middle, data.middle, "middle");
			up_down(&right, data.right, "right");

			move(&x, data.dx, cols);
			move(&y, -data.dy, rows);
		}
		
                for(i = 0; i < rows; i++)
                        for(j = 0; j < cols; j++)
                                if((cget(i, j, rows, cols) & 0xff00) == c)
                                        cput(i, j, oldc, rows, cols);
		
		if(num_read != sizeof(struct mouse_data))
			break;
		
                oldc = cget(y, x, rows, cols);
                cput(y, x, (oldc & 0x00ff) | c, rows, cols);
	}
	
	close(fd);
}
