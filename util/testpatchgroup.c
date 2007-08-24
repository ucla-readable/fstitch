#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <fscore/patchgroup.h>

static const char * result[] = { "FAIL", "PASS" };

int main(int argc, char ** argv)
{
	patchgroup_id_t a, b;
	int r;
	pid_t pid;

	a = patchgroup_create(0);
	printf("patchgroup_create(0) : a = %d [%s]\n", a, result[a==1]);
	b = patchgroup_create(0);
	printf("patchgroup_create(0) : b = %d [%s]\n", b, result[b==2]);

	r = patchgroup_release(b);
	printf("patchgroup_release(%d) : %d [%s]\n", b, r, result[r>=0]);

	r = patchgroup_add_depend(a, b);
	printf("patchgroup_add_depend(%d, %d) : %d [%s]\n", a, b, r, result[r>=0]);

	if (!(pid = fork()))
	{
		pid = getpid();
		r = patchgroup_add_depend(b, a);
		printf("[%08x] patchgroup_add_depend(%d, %d) : %d [%s]\n", pid, b, a, r, result[r<0]);

		r = patchgroup_abandon(a);
		printf("[%08x] patchgroup_abandon(%d) : %d [%s]\n", pid, a, r, result[r>=0]);
		return 0;
	}
	else if (pid < 0)
	{
		perror("fork");
		exit(1);
	}

	/* wait for a bit to help ensure parent and child printfs do not overlap */
	(void) usleep(1000000 / 5);

	r = patchgroup_release(a);
	printf("patchgroup_release(%d) : %d [%s]\n", a, r, result[r>=0]);

	r = patchgroup_engage(a);
	printf("patchgroup_engage(%d) : %d [%s]\n", a, r, result[r>=0]);
	r = patchgroup_engage(b);
	printf("patchgroup_engage(%d) : %d [%s]\n", b, r, result[r<0]);
	r = patchgroup_disengage(a);
	printf("patchgroup_disengage(%d) : %d [%s]\n", a, r, result[r>=0]);

	r = patchgroup_engage(a);
	printf("patchgroup_engage(%d) : %d [%s]\n", a, r, result[r>=0]);
	r = patchgroup_disengage(a);
	printf("patchgroup_disengage(%d) : %d [%s]\n", a, r, result[r>=0]);
	r = patchgroup_disengage(b);
	printf("patchgroup_disengage(%d) : %d [%s]\n", b, r, result[r>=0]);

	r = patchgroup_add_depend(a, b);
	printf("patchgroup_add_depend(%d, %d) : %d [%s]\n", a, b, r, result[r<0]);

	r = patchgroup_abandon(a);
	printf("patchgroup_abandon(%d) : %d [%s]\n", a, r, result[r>=0]);
	r = patchgroup_abandon(b);
	printf("patchgroup_abandon(%d) : %d [%s]\n", b, r, result[r>=0]);

	r = patchgroup_add_depend(a, b);
	printf("patchgroup_add_depend(%d, %d) : %d [%s]\n", a, b, r, result[r<0]);

	return 0;
}
