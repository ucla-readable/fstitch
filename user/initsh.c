#include <inc/lib.h>
		
void
umain(int argc, char **argv)
{
	int r;

	binaryname = "initsh";
	sys_env_set_name(0, "initsh");

	for (;;) {
		printf("initsh: starting sh\n");
		r = spawnl("/sh", "sh", (char*)0);
		if (r < 0) {
			fprintf(STDERR_FILENO, "initsh: spawn sh: %e\n", r);
			continue;
		}
		wait(r);
	}
}
