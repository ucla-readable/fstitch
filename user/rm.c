#include <inc/lib.h>

void
umain(int argc, char **argv)
{
	int i;
	for(i = 1; i < argc; i++)
	{
		int r = remove(argv[i]);
		if(r)
			printf("%s: %i\n", argv[i], r);
	}
}
