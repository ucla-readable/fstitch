#include <inc/lib.h>

static char data[4096];

void umain(int argc, char **argv)
{
	int i;
	for(i = 1; i < argc; i++)
	{
		int size, fid = open(argv[i], O_CREAT | O_WRONLY);
		if(fid < 0)
		{
			printf("open %s: %i\n", argv[i], fid);
			return;
		}

		for(size = 0; size < 4 * 1024 * 1024; )
		{
			int s = write(fid, &data, sizeof(data));
			if(s <= 0)
			{
				printf("write %s: %i\n", argv[i], s);
				return;
			}
			size += s;
		}
		printf("wrote %d bytes for %s\n", size, argv[i]);
	}
}
