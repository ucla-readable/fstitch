#include <inc/lib.h>

enum MARIO_POSE {
	MARIO_SQUAT = 0,
	MARIO_STAND,
	MARIO_SKID,
	MARIO_STEP1,
	MARIO_STEP2,
	MARIO_STEP3,
	MARIO_JUMP,
	/* must be last */
	MARIO_POSE_MAX
};

enum GOOMBA_POSE {
	GOOMBA_WALK1,
	GOOMBA_WALK2,
	GOOMBA_FLAT,
	/* must be last */
	GOOMBA_POSE_MAX
};

/* must use y, x for proper memory layout */
static uint8_t _mario_pose[MARIO_POSE_MAX][32][16];
static uint8_t _goomba_pose[GOOMBA_POSE_MAX][16][16];
static uint8_t _vga_buffer[200][320];
#define mario_pose(p, x, y) _mario_pose[p][y][x]
#define goomba_pose(p, x, y) _goomba_pose[p][y][x]
#define vga_buffer(x, y) _vga_buffer[y][x]

static void draw_mario(int x, int y, enum MARIO_POSE pose, int reverse)
{
	int px, py;
	
	if(pose < 0 || pose >= MARIO_POSE_MAX)
		return;
	for(px = 0; px != 16; px++)
	{
		int vx = x + px;
		if(vx < 0 || vx >= 320)
			continue;
		for(py = 0; py != 32; py++)
		{
			int vy = y + py;
			if(vy < 0 || vy >= 200)
				continue;
			if(mario_pose(pose, reverse ? 15 - px : px, py))
				vga_buffer(vx, vy) = mario_pose(pose, reverse ? 15 - px : px, py);
		}
	}
}

static void draw_goomba(int x, int y, enum GOOMBA_POSE pose)
{
	int px, py;
	
	if(pose < 0 || pose >= GOOMBA_POSE_MAX)
		return;
	for(px = 0; px != 16; px++)
	{
		int vx = x + px;
		if(vx < 0 || vx >= 320)
			continue;
		for(py = 0; py != 16; py++)
		{
			int vy = y + py;
			if(vy < 0 || vy >= 200)
				continue;
			if(goomba_pose(pose, px, py))
				vga_buffer(vx, vy) = goomba_pose(pose, px, py);
		}
	}
}

#define SCALE 10

static void play_mario(void)
{
	int input;
	
	/* init mario state */
	int mario_x = 0;
	int mario_y = (200 - 32) * SCALE;
	enum MARIO_POSE mario_pose = MARIO_STAND;
	int mario_left = 0;
	int mario_vx = 0;
	int mario_vy = 0;
	int mario_ax = 0;
	int mario_ticks = 0;
	
	/* init goomba state */
	int goomba_x = (320 - 16) * SCALE;
	int goomba_y = (200 - 16) * SCALE;
	enum GOOMBA_POSE goomba_pose = GOOMBA_WALK1;
	int goomba_v = -1;
	int goomba_ticks = 0;
	int goomba_spawn = 0;
	
	input = sys_cgetc_nb();
	while(input != 'q' && input != 'Q' && input != 27)
	{
		switch(input)
		{
			case ' ':
			case KEYCODE_UP:
				if(!mario_vy)
					mario_vy = 35; /* jump */
				break;
			case KEYCODE_LEFT:
				if(mario_ax < 0)
				{
					mario_ax -= 5;
					if(mario_ax < -10)
						mario_ax = -10;
				}
				else
					mario_ax = -5;
				mario_left = 1;
				break;
			case KEYCODE_RIGHT:
				if(mario_ax > 0)
				{
					mario_ax += 5;
					if(mario_ax > 10)
						mario_ax = 10;
				}
				else
					mario_ax = 5;
				mario_left = 0;
				break;
			default:
				if(mario_ax < 0)
					mario_ax++;
				else if(mario_ax > 0)
					mario_ax--;
		}
		
		/* update mario x */
		mario_x += mario_vx;
		mario_vx += mario_ax;
		if(mario_vx < 0)
			mario_vx++;
		else if(mario_vx > 0)
			mario_vx--;
		/* max velocity */
		if(mario_vx > 20)
			mario_vx = 10;
		if(mario_vx < -20)
			mario_vx = -10;
		if(mario_x < 0)
		{
			mario_x = 0;
			mario_vx = 0;
		}
		else if(mario_x > (320 - 16) * SCALE)
		{
			mario_x = (320 - 16) * SCALE;
			mario_vx = 0;
		}
		/* update mario y */
		mario_y -= mario_vy;
		mario_vy--; /* gravity */
		if(mario_y < 0)
		{
			mario_y = 0;
			mario_vy = 0;
		}
		else if(mario_y >= (200 - 32) * SCALE)
		{
			mario_y = (200 - 32) * SCALE;
			mario_vy = 0;
		}
		
		/* what is mario's position? */
		if(mario_vy)
			mario_pose = MARIO_JUMP;
		else if(!mario_vx && !mario_ax)
			mario_pose = MARIO_STAND;
		else if(mario_ax < 0 && mario_vx >= 0)
		{
			mario_pose = MARIO_SKID;
			mario_left = 0;
		}
		else if(mario_ax > 0 && mario_vx <= 0)
		{
			mario_pose = MARIO_SKID;
			mario_left = 1;
		}
		else
		{
			if(mario_pose != MARIO_STEP1 && mario_pose != MARIO_STEP2 && mario_pose != MARIO_STEP3)
			{
				mario_pose = MARIO_STEP1;
				mario_ticks = 0;
			}
			else if(++mario_ticks >= 25 - mario_vx / 2)
			{
				if(mario_pose == MARIO_STEP3)
					mario_pose = MARIO_STEP1;
				else
					mario_pose++;
			}
		}
		
		goomba_x += goomba_v;
		if(goomba_x < 0 || goomba_x > (320 - 16) * SCALE)
		{
			goomba_v = -goomba_v;
			goomba_x += goomba_v;
		}
		if(++goomba_ticks == 20)
		{
			goomba_ticks = 0;
			if(goomba_pose == GOOMBA_WALK1)
				goomba_pose = GOOMBA_WALK2;
			else if(goomba_pose == GOOMBA_WALK2)
				goomba_pose = GOOMBA_WALK1;
			else
			{
				if(++goomba_spawn == 10)
				{
					goomba_x = (320 - 16) * SCALE;
					goomba_v = -1;
					goomba_pose = GOOMBA_WALK1;
					goomba_spawn = 0;
				}
			}
		}
		
		/* did we step on the goomba? */
		input = mario_x - goomba_x;
		if(-10 * SCALE < input && input < 10 * SCALE)
		{
			if(goomba_y <= (mario_y + 32 * SCALE) && mario_vy < 0)
			{
				goomba_v = 0;
				goomba_pose = GOOMBA_FLAT;
			}
		}
		
		memset(_vga_buffer, 0x1F, 64000);
		
		draw_goomba(goomba_x / SCALE, goomba_y / SCALE, goomba_pose);
		draw_mario(mario_x / SCALE, mario_y / SCALE, mario_pose, mario_left);
		
		memcpy((void *) 0xA0000, _vga_buffer, 64000);
		
		input = sys_cgetc_nb();
	}
}

/* get the filesystem server to cache these files */
static void preload_files(char * prefix)
{
	char filename[MAXNAMELEN];
	int i, fd;
	
	printf("Preloading data... ");
	for(i = 0; i != 10; i++)
	{
		snprintf(filename, MAXNAMELEN, "%s.%d", prefix, i);
		fd = open(filename, O_RDONLY);
		if(fd < 0)
			break;
		close(fd);
	}
	printf("done.\n");
}

void umain(int argc, char * argv[])
{
	uint8_t palette[768];
	envid_t envid;
	const struct Env * e;
	
	int r, fd = open("/mario.pal", O_RDONLY);
	memset(palette, 0, 768);
	r = read(fd, palette, 768);
	close(fd);
	printf("mario.pal: %d colors\n", r / 3);
	
	fd = open("/mario.spr", O_RDONLY);
	r = read(fd, _mario_pose, sizeof(_mario_pose));
	close(fd);
	printf("mario.spr: %d sprites\n", r / sizeof(_mario_pose[0]));
	
	fd = open("/goomba.spr", O_RDONLY);
	r = read(fd, _goomba_pose, sizeof(_goomba_pose));
	close(fd);
	printf("goomba.spr: %d sprites\n", r / sizeof(_goomba_pose[0]));
	
	preload_files("mario");
	
	/* adjust the palette */
	for(r = 0; r != 768; r++)
		palette[r] >>= 2;
	
	/* go to graphics mode! */
	if(sys_vga_set_mode_320(0xA0000, 0) < 0)
		exit(1);
	sys_vga_set_palette(palette, 0);
	
	/* spawn some music */
	envid = spawnl("/sb16", "/sb16", "mario", NULL);
	e = &envs[ENVX(envid)];
	
	play_mario();
	
	if(e->env_id == envid && e->env_status != ENV_FREE)
	{
		sys_env_destroy(envid);
		/* close it for the late sb16 environment */
		sys_sb16_close();
	}
	
	/* restore text */
	sys_vga_set_mode_text(0);
}
