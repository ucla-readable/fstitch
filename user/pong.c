#include <inc/lib.h>

#define VGA 0xA0000

/* a general purpose pseudorandom number generator */
static int rand(int nseed)
{
	static int seed = 0;
	if(nseed)
		seed = nseed;
	seed *= 214013;
	seed += 2531011;
	return (seed >> 16) & 0x7fff;
}

static void parent_set_video(int eid)
{
	rand(hwclock_time(NULL));
	sys_vga_set_mode_320(VGA);
	sys_vga_set_mode_text();
	ipc_send(eid, 0, NULL, 0, NULL);
	ipc_recv(eid, NULL, NULL, NULL, NULL, 0);
}

static void child_set_video(void)
{
	int parent;
	ipc_recv(0, &parent, NULL, NULL, NULL, 0);
	sys_vga_set_mode_320(VGA);
	ipc_send(parent, 0, NULL, 0, NULL);
	rand(hwclock_time(NULL));
}

static void pong(int child)
{
	int x = rand(0) % 320, y = rand(0) % 200;
	int dx = 1, dy = 1;
	char old_pixel, color = child ? 255 : 128;
	char * vga = (char *) VGA;
	
	if(rand(0) % 2)
		dx = -1;
	if(rand(0) % 2)
		dy = -1;
	
	old_pixel = vga[y * 320 + x];
	vga[y * 320 + x] = color;
	
	for(;;)
	{
		int old_index = y * 320 + x;
		int new_index, old_priority;
		
		/* play pong! */
		x += dx;
		if(x < 0 || x == 320)
		{
			dx = -dx;
			x += 2 * dx;
		}
		y += dy;
		if(y < 0 || y == 200)
		{
			dy = -dy;
			y += 2 * dy;
		}
		
		new_index = y * 320 + x;
		old_priority = env->env_rpriority;
		sys_env_set_priority(0, ENV_MAX_PRIORITY);
		if(vga[old_index] == color)
			vga[old_index] = old_pixel;
		sys_env_set_priority(0, old_priority);
		old_pixel = vga[new_index];
		vga[new_index] = color;
		
		if(child && sys_cgetc_nb() > 0)
		{
			sys_env_destroy(child);
			sys_vga_set_mode_text();
			exit();
		}
		
		sleep(child ? 2 : 1);
	}
}

void umain(void)
{
	int eid = fork();
	if(eid < 0)
	{
		fprintf(STDERR_FILENO, "fork: %e\n", eid);
		return;
	}
	if(eid)
		parent_set_video(eid);
	else
		child_set_video();
	pong(eid);
}
