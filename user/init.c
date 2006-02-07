#include <inc/lib.h>

void
umain(void)
{
	int r;

	binaryname = "init";
	sys_env_set_name(0, "init");

	if ((r = spawnl("/sh", "/sh", "/init.sh", (const char **) 0)) < 0)
		kdprintf(STDERR_FILENO, "[%08x] spawn /init.sh: %e\n", env->env_id, r);
}
