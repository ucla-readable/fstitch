#include <inc/lib.h>

static void
print_usage(char *bin)
{
	printf("%s: <dir> [<dir> ...]\n", bin);
}

void
umain(int argc, char **argv)
{
	if(argc < 2)
	{
		print_usage(argv[0]);
		exit();
	}

	int i;
	for(i=1; i<argc; i++)
	{
		int r;
		int fd;
		if((fd = r = open(argv[i], O_CREAT | O_MKDIR)) < 0)
		{
			kdprintf(STDERR_FILENO, "open(%s): %e\n", argv[i], r);
			exit();
		}

		// Ensure the directory was created, in case of buggy fs
		struct Stat s;
		if((r = fstat(fd, &s)) < 0)
			panic("fstat: %e", r);
		assert(s.st_isdir);
	}
}
