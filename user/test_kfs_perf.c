#include <stdio.h>
#include <arch/simple.h>

extern int perf_test(int cfs_bd, const char * file, int size);

void umain(int argc, const char ** argv)
{
	const char * file = "perf";
	int size = 4*1024*1024;
	int time;

	if (get_arg_idx(argc, argv, "-h"))
	{
		printf("Usage: %s [test_file] [size]\n");
		exit();
	}

	if (argc >= 2)
		file = argv[1];
	if (argc >= 3)
		size = strtol(argv[2], NULL, 10);

	time = perf_test(0, size, file);
	if (time > 0)
		printf("%u kBps\n", 4*1024 / (time / 100));
	else
		printf("perf_test: %e\n", time);
}
