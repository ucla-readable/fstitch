#include <inc/lib.h>
#include <inc/mouse.h>

static envid_t
find_moused(void)
{
	size_t ntries;
	size_t i;

	// Try to find fs a few times, in case this env is being
	// started at the same time as moused, thus giving moused time to do its
	// fork.
	// 20 is most arbitrary: 10 worked in bochs, so I doubled to get 20.
	// NOTE: netclient.c:find_netd_ipcrecv() does the same.
	for (ntries = 0; ntries < 20; ntries++)
	{
		for (i = 0; i < NENV; i++)
		{
			//if (envs[i].env_status != ENV_FREE)
			//	printf("find_moused: name: [%s]\n", envs[i].env_name);
			if (envs[i].env_status != ENV_FREE &&
				(!strncmp(envs[i].env_name, "moused", 6)
				 || !strncmp(envs[i].env_name, "/moused", 7)))
				return envs[i].env_id;
		}
		sys_yield();
	}

	return 0;
}

#define CONSOLE 0xB8000

static uint16_t *console = (uint16_t *) CONSOLE;

static inline void
cput(int row, int col, uint16_t c, int rows, int cols)
{
        assert(row >= 0 && row < rows);
        assert(col >= 0 && col < cols);
        console[row * 80 + col] = c;
}

static inline uint16_t
cget(int row, int col, int rows, int cols)
{
        assert(row >= 0 && row < rows);
        assert(col >= 0 && col < cols);
        return console[row * 80 + col];
}

static inline void
up_down(int *x0, int x1, const char *name)
{
	if (*x0 == x1)
		return;
	//printf("(%s %s)", name, x1 ? "down" : "up");
	/* have some fun! start the fall demo on mouse up */
	if(!x1)
		spawnl("/demo", "/demo", "fall", NULL);
	*x0 = x1;
}

static inline void
move(int *x, int dx, int upper_bound)
{
	*x += dx;
	if (*x < 0)
		*x = 0;
	else if (*x >= upper_bound)
		*x = upper_bound - 1;
}

void
umain(void)
{
	int moused = find_moused();
	if (moused == 0)
	{
		printf("error: can't find moused\n");
		return;
	}

	ipc_send(moused, 0, NULL, 0, NULL);
	int fd = dup2env_recv(moused);
	if (fd < 0)
	{
		printf("error: unable to connect to moused\n");
		return;
	}

	int cols = 80, rows = sys_vga_map_text(CONSOLE);
	if (rows < 0)
	{
		printf("error: unable to map console memory\n");
		return;
	}

	int x = 0, y = 0, c = 0x7000, oldc = -1,
	    left = 0, middle = 0, right = 0;

	while (1)
	{
		struct mouse_data data;
		int num_read = 0;
		while (num_read != sizeof(struct mouse_data))
		{
			int n = read(fd, ((uint8_t *) &data) + num_read,
				     sizeof(struct mouse_data) - num_read);
			if (n <= 0)
				sys_yield();
			else
				num_read += n;
		}

		up_down(&left, data.left, "left");
		up_down(&middle, data.middle, "middle");
		up_down(&right, data.right, "right");

		move(&x, data.dx, cols);
		move(&y, -data.dy, rows);

		int i;
                for (i = 0; i < rows; i++)
		{
			int j;
                        for (j = 0; j < cols; j++)
                                if ((cget(i, j, rows, cols) & 0xff00) == c)
                                        cput(i, j, oldc, rows, cols);
		}

                oldc = cget(y, x, rows, cols);
                cput(y, x, (oldc & 0x00ff) | c, rows, cols);
	}

	close(fd);
}

