#include <inc/lib.h>

static char buf[8192];

static int
cat(int f, char *s, bool term)
{
	long n;
	int r;

	while((n = (term ? read : readn)(f, buf, sizeof(buf))) > 0)
		if((r = write(STDOUT_FILENO, buf, n)) != n)
			return (r < 0) ? r : -E_NO_DISK;
	if(n < 0)
		panic("error reading %s: %i", s, n);
	return 0;
}

void
umain(int argc, char **argv)
{
	int i, r = 0;

	argv0 = "cat";
	if(argc == 1)
		r = cat(0, "<stdin>", 1);
	else for(i = 1; i < argc && !r; i++)
	{
		if(!strcmp(argv[i], "-"))
			r = cat(0, "<stdin>", 0);
		else
		{
			int f = open(argv[i], O_RDONLY);
			if(f < 0)
			{
				kdprintf(STDERR_FILENO, "can't open %s: %i\n", argv[i], f);
				exit(0);
			}
			else
			{
				r = cat(f, argv[i], 0);
				close(f);
			}
		}
	}
	if(r)
		kdprintf(STDERR_FILENO, "write error: %i\n", r);
}
