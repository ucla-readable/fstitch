#include <inc/lib.h>

int
is_descendent(envid_t e, envid_t descendent)
{
	const envid_t descendent_parent = envs[ENVX(descendent)].env_parent_id;

	if(descendent_parent == e)
		return 1;

	// envs created by the kernel have parent envid_t 0.
	// Thus if we have reached envid 0, we are at the top and have not found
	// a match.
	if(descendent_parent == 0)
		return 0;

	// If we happen upon a parent env that no longer exists always say
	// "yes". Without this parent to traverse we can't really tell if
	// descendent is a descendent of e, yes errors on the always-list side.
	if(envs[ENVX(descendent_parent)].env_status == ENV_FREE
		|| descendent_parent != envs[ENVX(descendent_parent)].env_id)
		return 1;

	if(envs[ENVX(descendent_parent)].env_status != ENV_FREE)
		return is_descendent(e, descendent_parent);

	return 0;
}
	
void
print_envs(envid_t root_envid)
{
	int i;

	printf("    envid     parent  S   pri   d(last)      runs   TSC util  name\n");

	for(i=0; i < NENV; i++)
	{
		const struct Env *e = &envs[i];
		if(e->env_status == ENV_FREE)
			continue;
		if(root_envid != e->env_id && !is_descendent(root_envid, e->env_id))
			continue;

		printf("[%08x] [%08x] ", e->env_id, e->env_parent_id);

		switch(e->env_status)
		{
			case(ENV_RUNNABLE):     printf("r"); break;
			case(ENV_NOT_RUNNABLE): printf("N"); break;
			case(ENV_FREE):         printf("F"); break;
			default:                printf("?");
		}

		printf(" %02d/%02d", e->env_epriority, e->env_rpriority);

		printf(" %9d", env->env_jiffies - e->env_jiffies);

		printf(" %9d %5d", e->env_runs, (uint32_t) (e->env_tsc >> 26));
		if(e->env_runs)
			printf(" %4d", (uint32_t) ((e->env_tsc / e->env_runs) >> 8));
		else
			printf("   --");
		printf(" %c%s\n", (e->env_id == env->env_id) ? '*' : ' ', e->env_name);
	}
}

void
print_usage(const char *bin)
{
	printf("Usage: %s [root envid]\n", bin);
	printf("About: ps for environments.\n");
}

void
umain(int argc, char **argv)
{
	if(argc > 2 || (argc == 2 && !strcmp(argv[1], "-h")))
	{
		print_usage(argv[0]);
		exit();
	}

	envid_t root_envid;
	if(argc == 2)
		root_envid = strtol(argv[1], NULL, 16);
	else
		root_envid = 0;

	print_envs(root_envid);
}
