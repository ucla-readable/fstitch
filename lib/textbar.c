#include <inc/error.h>
#include <lib/types.h>
#include <inc/lib.h>

#define TEXT ((uint8_t *) 0xB8000)

static int textbar_line = -1;

int textbar_set_progress(int progress, uint8_t color)
{
	int i;
	
	if(textbar_line < 0)
		return -E_INVAL;
	if(progress < 0 || progress > 160)
		return -E_INVAL;
	
	progress--;
	for(i = 0; i != 160; i += 2)
	{
		uint8_t block;
		if(i < progress)
			/* double */
			block = 0xDB;
		else if(i > progress)
			/* empty */
			block = 0x20;
		else
			/* half */
			block = 0xDD;
		TEXT[textbar_line * 160 + i] = block;
		TEXT[textbar_line * 160 + i + 1] = color;
	}
	
	return 0;
}

int textbar_close(void)
{
	return textbar_set_progress(0, 7);
}

int textbar_init(int use_line)
{
	int r = sys_vga_map_text(0xB8000);
	if(r < 0)
		return r;
	/* assume that there are actually r + 1 lines on the screen */
	if(0 <= use_line && use_line <= r)
		textbar_line = use_line;
	else
		textbar_line = r;
	textbar_set_progress(0, 7);
	return 160;
}
