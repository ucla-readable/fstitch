#include <inc/lib.h>

void
umain(int argc, char **argv)
{
	int i;
	for(i = 1; i < argc; i++)
	{
		int r = rmdir(argv[i]);
		if(r)
			printf("%s: %e\n", argv[i], r);
	}
}
