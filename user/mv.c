#include <inc/lib.h>

void
umain(int argc, char **argv)
{
	int r;
	if (argc != 3) {
		printf("Usage: %s source dest\n", argv[0]);
		return;
	}

	r = rename(argv[1], argv[2]);
	if(r) {
		printf("%s: %e\n", argv[1], r);
	}
}
