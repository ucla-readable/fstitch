#ifdef KUDOS
#include <inc/lib.h>
#else
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#include <kfs/opgroup.h>

static const char * result[] = { "FAIL", "PASS" };

#if defined(KUDOS)
void umain(int argc, char ** argv)
#elif defined(KERNEL_USER)
int main(int argc, char ** argv)
#else
#error Unknown target
#endif
{
	opgroup_id_t a, b;
	int r;
#if defined(KUDOS)
	envid_t pid;
#elif defined(KERNEL_USER)
	pid_t pid;
#else
#error Unknown target
#endif

	a = opgroup_create(0);
	printf("opgroup_create(0) : a = %d [%s]\n", a, result[a==1]);
	b = opgroup_create(0);
	printf("opgroup_create(0) : b = %d [%s]\n", b, result[b==2]);

	r = opgroup_release(b);
	printf("opgroup_release(%d) : %d [%s]\n", b, r, result[r>=0]);

	r = opgroup_add_depend(a, b);
	printf("opgroup_add_depend(%d, %d) : %d [%s]\n", a, b, r, result[r>=0]);

	if (!(pid = fork()))
	{
#if defined(KUDOS)
		pid = env->env_id;
#elif defined(KERNEL_USER)
		pid = getpid();
#else
#error Unknown target
#endif
		r = opgroup_add_depend(b, a);
		printf("[%08x] opgroup_add_depend(%d, %d) : %d [%s]\n", pid, b, a, r, result[r<0]);

		r = opgroup_abandon(a);
		printf("[%08x] opgroup_abandon(%d) : %d [%s]\n", pid, a, r, result[r>=0]);
		return;
	}
	else if (pid < 0)
	{
#if defined(KUDOS)
		panic("fork(): %i\n", pid);
#elif defined(KERNEL_USER)
		perror("fork");
		exit(1);
#else
#error Unknown target
#endif
	}

	// wait for a bit to help ensure parent and child printfs do not overlap
#if defined(KUDOS)
	(void) jsleep(HZ / 5);
#elif defined(KERNEL_USER)
	(void) usleep(1000000 / 5);
#else
#error Unknown target
#endif

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

#ifdef KERNEL_USER
	return 0;
#endif
}
