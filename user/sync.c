#include <inc/lib.h>
#include <inc/cfs_ipc_client.h>

void umain(int argc, char * argv[])
{
	int r;
	if(argc <= 1)
	{
		printf("Syncing filesystem... ");
		r = sync();
		if(r < 0)
			printf("%e\n", r);
		else
			printf("done.\n");
	}
	else
	{
		int i;
		for(i = 1; i < argc; i++)
		{
			if(argv[i][0] != '/')
			{
				printf("Not an absolute path: %s\n", argv[i]);
				continue;
			}
			printf("Syncing %s... ", argv[i]);
			/* have to go under the hood for this one */
			r = cfs_sync(argv[i]);
			if(r < 0)
				printf("%e\n", r);
			else
				printf("done.\n");
		}
	}
}
