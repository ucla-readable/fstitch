#include <inc/lib.h>
		
void
umain(int argc, char **argv)
{
	int r;

	binaryname = "initsh";
	sys_env_set_name(0, "initsh");
	//printf("initsh: running sh\n");

	// being run directly from kernel, so no file descriptors open yet
	close(0);
	if ((r = opencons()) < 0)
		panic("opencons: %e", r);
	if (r != STDIN_FILENO)
		panic("first opencons used fd %d", r);
	if ((r = dup2(STDIN_FILENO, STDOUT_FILENO)) < 0)
		panic("dup2: %e", r);
	if ((r = dup2(STDOUT_FILENO, STDERR_FILENO)) < 0)
		panic("dup2: %e", r);
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
