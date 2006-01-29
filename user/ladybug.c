#include <inc/lib.h>

/* from demo.c */
extern uint8_t demo_buffer[2][64000];

void ladybug(int argc, char * argv[])
{
	char * base = "lady";
	char filename[128];
	bool use_palette = 0;
	int r, i;
	int fd;

	if(argc > 1)
		base = argv[1];

	snprintf(filename, sizeof(filename), "/%s.pal", base);
	fd = open(filename, O_RDONLY);
	if(fd >= 0)
	{
		r = read(fd, demo_buffer[1], 256 * 3);
		if(r == 256 * 3)
			use_palette = 1;
		close(fd);
	}

	if(use_palette)
		for(i = 0; i < (256*3); i++)
			demo_buffer[1][i] >>= 2;

	snprintf(filename, sizeof(filename), "/%s.img", base);
	fd = open(filename, O_RDONLY);
	r = read(fd, demo_buffer[0], 200 * 320);
	close(fd);

	// set graphics mode
	if(sys_vga_set_mode_320(0xA0000) < 0)
		exit(1);
	if(use_palette)
		sys_vga_set_palette(demo_buffer[1], 0);
	
	memcpy((void *) 0xA0000, demo_buffer[0], 64000);

	getchar();

	// restore text mode
	sys_vga_set_mode_text();
}
