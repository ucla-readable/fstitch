#include <inc/lib.h>
#include <kfs/opgroup.h>

static const char * result[] = { "FAIL", "PASS" };

void umain(int argc, char ** argv)
{
	opgroup_id_t a, b;
	int r;
	envid_t envid;

	a = opgroup_create(0);
	printf("opgroup_create(0) : a = %d [%s]\n", a, result[a==1]);
	b = opgroup_create(0);
	printf("opgroup_create(0) : b = %d [%s]\n", b, result[b==2]);

	r = opgroup_release(b);
	printf("opgroup_release(%d) : %d [%s]\n", b, r, result[r>=0]);

	r = opgroup_add_depend(a, b);
	printf("opgroup_add_depend(%d, %d) : %d [%s]\n", a, b, r, result[r>=0]);
	
	if (!(envid = fork()))
	{
		r = opgroup_add_depend(b, a);
		printf("[%08x] opgroup_add_depend(%d, %d) : %d [%s]\n", env->env_id, b, a, r, result[r<0]);

		r = opgroup_abandon(a);
		printf("[%08x] opgroup_abandon(%d) : %d [%s]\n", env->env_id, a, r, result[r>=0]);
		return;
	}
	else if (envid < 0)
		panic("fork(): %e\n", envid);

	// wait for a bit to help ensure parent and child printfs do not overlap
	(void) jsleep(HZ / 5); 

	r = opgroup_release(a);
	printf("opgroup_release(%d) : %d [%s]\n", a, r, result[r>=0]);

	r = opgroup_engage(a);
	printf("opgroup_engage(%d) : %d [%s]\n", a, r, result[r>=0]);
	r = opgroup_engage(b);
	printf("opgroup_engage(%d) : %d [%s]\n", b, r, result[r<0]);
	r = opgroup_disengage(a);
	printf("opgroup_disengage(%d) : %d [%s]\n", a, r, result[r>=0]);

	r = opgroup_engage(a);
	printf("opgroup_engage(%d) : %d [%s]\n", a, r, result[r>=0]);
	r = opgroup_disengage(a);
	printf("opgroup_disengage(%d) : %d [%s]\n", a, r, result[r>=0]);
	r = opgroup_disengage(b);
	printf("opgroup_disengage(%d) : %d [%s]\n", b, r, result[r>=0]);

	r = opgroup_add_depend(a, b);
	printf("opgroup_add_depend(%d, %d) : %d [%s]\n", a, b, r, result[r<0]);

	r = opgroup_abandon(a);
	printf("opgroup_abandon(%d) : %d [%s]\n", a, r, result[r>=0]);
	r = opgroup_abandon(b);
	printf("opgroup_abandon(%d) : %d [%s]\n", b, r, result[r>=0]);

	r = opgroup_add_depend(a, b);
	printf("opgroup_add_depend(%d, %d) : %d [%s]\n", a, b, r, result[r<0]);
}
