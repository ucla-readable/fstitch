#include <inc/lib.h>

void
umain(int argc, char **argv)
{
	int r;

	if (argc != 2)
	{
		printf("Usage: %s <secs>\n", argv[0]);
		exit();
	}

	const int32_t centisecs = 100*strtol(argv[1], NULL, 10);

	if ((r = sleep(centisecs)) < 0)
	{
		fprintf(STDERR_FILENO, "sleep: %e\n", r);
		exit();
	}
}
