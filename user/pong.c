#include <inc/lib.h>

/* from demo.c */
int rand(int nseed);

#define VGA 0xA0000

static void parent_set_video(int eid)
{
	int value;
	rand(hwclock_time(NULL));
	if(sys_vga_set_mode_320(VGA, 0))
	{
		ipc_send(eid, 1, NULL, 0, NULL);
		exit(1);
	}
	sys_vga_set_mode_text(0);
	ipc_send(eid, 0, NULL, 0, NULL);
	value = ipc_recv(eid, NULL, NULL, NULL, NULL, 0);
	if(value)
		exit(1);
}

static void child_set_video(void)
{
	int value, parent;
	value = ipc_recv(0, &parent, NULL, NULL, NULL, 0);
	if(value)
		exit(1);
	if(sys_vga_set_mode_320(VGA, 0) < 0)
	{
		ipc_send(parent, 1, NULL, 0, NULL);
		exit(1);
	}
	ipc_send(parent, 0, NULL, 0, NULL);
	rand(hwclock_time(NULL));
}

static void playpong(int child)
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
			sys_vga_set_mode_text(0);
			exit(0);
		}
		
		jsleep(child ? (HZ / 50) : (HZ / 100));
	}
}

void pong(void)
{
	int eid = fork();
	if(eid < 0)
	{
		kdprintf(STDERR_FILENO, "fork: %i\n", eid);
		return;
	}
	if(eid)
		parent_set_video(eid);
	else
		child_set_video();
	playpong(eid);
}
