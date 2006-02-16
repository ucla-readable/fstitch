#include <inc/lib.h>
#include <inc/sb16.h>

void
umain(int argc, char * argv[])
{
	int volume, error;
	
	if(argc != 2)
	{
		printf("Usage: %s <volume>\n", argv[0]);
		return;
	}
	
	volume = strtol(argv[1], NULL, 10);
	
	error = sys_sb16_open(44100, 1, (uintptr_t) SB16_USER_BUFFER);
	if(error)
	{
		printf("sys_sb16_open: %i\n", error);
		return;
	}
	
	sys_sb16_setvolume(volume);
	printf("Set volume to %d/100\n", volume);
	
	sys_sb16_close();
}
