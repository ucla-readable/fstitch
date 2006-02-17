#include <inc/lib.h>

void
umain(int argc, char **argv)
{
	int r;

	if (argc != 2)
	{
		printf("Usage: %s <secs>\n", argv[0]);
		exit(0);
	}

	const int32_t jiffies = HZ*strtol(argv[1], NULL, 10);

	if ((r = jsleep(jiffies)) < 0)
	{
		kdprintf(STDERR_FILENO, "jsleep: %i\n", r);
		exit(0);
	}
}
