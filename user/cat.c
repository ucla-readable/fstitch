
#include <inc/lib.h>

char buf[8192];

int
cat(int f, char *s)
{
	long n;
	int r;

	while ((n = read(f, buf, (long) sizeof(buf))) > 0)
		if ((r = write(1, buf, n)) != n)
			return (r < 0) ? r : -E_NO_DISK;
	if(n < 0)
		panic("error reading %s: %e", s, n);
	return 0;
}

void
umain(int argc, char **argv)
{
	int i, r = 0;

	argv0 = "cat";
	if(argc == 1)
		r = cat(0, "<stdin>");
	else for(i = 1; i < argc && !r; i++)
	{
		int f = open(argv[i], O_RDONLY);
		if(f < 0)
		{
			fprintf(STDERR_FILENO, "can't open %s: %e\n", argv[i], f);
			exit();
		}
		else
		{
			r = cat(f, argv[i]);
			close(f);
		}
	}
	if(r)
		fprintf(STDERR_FILENO, "write error: %e\n", r);
}
