#include <stdio.h>
#include <arch/simple.h>

extern int perf_test(int cfs_bd, const char * file, int size);

void umain(int argc, const char ** argv)
{
	const char * file = "perf";
	int size = 4*1024*1024;
	int time;

	if (argc < 2 || get_arg_idx(argc, argv, "-h"))
	{
		printf("Usage: %s [test_file] [size]\n", argv[0]);
		exit(0);
	}

	if (argc >= 2)
		file = argv[1];
	if (argc >= 3)
		size = strtol(argv[2], NULL, 10);

	time = perf_test(0, file, size);
	if (time > 0)
		printf("%u kBps\n", (unsigned) (4*1024 / ((double)time / HZ)));
	else
		printf("perf_test: %e\n", time);
}
