#include <inc/lib.h>

void
umain(int argc, char **argv)
{
	int i, j, s;
	char data[512];
	int size;
	for(i = 1; i < argc; i++) {
		int fid = open(argv[i], O_CREAT|O_WRONLY);
		if(fid < 0)
		{
			printf("open %s: %e\n", argv[i], fid);
			return;
		}

		size = 0;
		for(j = 0; j < 8192; j++) {
			s = write(fid, &data, 512);
			if (s < 0) {
				printf("write %s: %e\n", argv[i], s);
				return;
			}
			size += s;
		}
		printf("wrote %d bytes for %s\n", size, argv[i]);
	}
}
