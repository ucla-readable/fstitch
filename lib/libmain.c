// Called from entry.S to get us going.
// entry.S already took care of defining envs, pages, vpd, and vpt.

#include <inc/lib.h>

extern void umain(int, char**);

const struct Env *env;
char *binaryname = "(PROGRAM NAME UNKNOWN)";

void
libmain(int argc, char **argv)
{
	// set env to point at our env structure in envs[].
	env = &envs[ENVX(sys_getenvid())];

	// save the name of the program so that panic() can use it
	if (argc > 0)
	{
		binaryname = argv[0];
		sys_env_set_name(0, argv[0]);
	}

	// ensure stdin, stdout, and stderr fds exist
	int r, consfd;
	if((consfd = r = opencons()) < 0)
		panic("opencons: %e", r);
	if(consfd != STDIN_FILENO && consfd < STDERR_FILENO)
		panic("some but not all standard fds are present");
	if(consfd == STDIN_FILENO)
	{
		// std fds not allocated, set them up
		if((r = dup2(STDIN_FILENO, STDOUT_FILENO)) < 0)
			panic("dup2(STDIN_FILENO, STDOUT_FILENO): %e", r);
		if((r = dup2(STDIN_FILENO, STDERR_FILENO)) < 0)
			panic("dup2(STDIN_FILENO, STDERR_FILENO): %e", r);
	}
	else
	{
		// std fds are allocated
		if((r = close(consfd)) < 0)
			panic("close(%d): %e", consfd, r);
	}

#if ENABLE_ENV_SYMS
	// set the kernel's symbol and symbol string tables
	extern uint8_t _binary_symtbl_start[], _binary_symtbl_size[];
	extern uint8_t _binary_symstrtbl_start[], _binary_symstrtbl_size[];
	if ((r = sys_set_symtbls(0,
									 (void*)  _binary_symtbl_start,
									 (size_t) _binary_symtbl_size,
									 (void*)  _binary_symstrtbl_start,
									 (size_t) _binary_symstrtbl_size)) < 0)
		panic("sys_set_symtbls: %e", r);
#endif

	// call user main routine
	umain(argc, argv);

	// exit gracefully
	exit();
}

