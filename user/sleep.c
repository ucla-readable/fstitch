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

	const int32_t jiffies = HZ*strtol(argv[1], NULL, 10);

	if ((r = sleepj(jiffies)) < 0)
	{
		kdprintf(STDERR_FILENO, "sleepj: %e\n", r);
		exit();
	}
}
