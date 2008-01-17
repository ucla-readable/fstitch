#include "fscore/patchgroup.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		printf("No command was given\n");
		return 1;
	}

	if (!strcmp(argv[1], "start"))
	{
		if (argc < 3)
		{
			printf("No path was given\n");
			return 1;
		}
		if (argc < 4)
		{
			printf("No program was given\n");
			return 1;
		}
		int r = txn_start(argv[2]);
		printf("txn_start(\"%s\") = %s\n", argv[2], strerror(errno));

		if (!r)
		{
			execvp(argv[3], &argv[3]);
			printf("execvp() = %s\n", strerror(errno));
			txn_finish();
		}
	}
	else if (!strcmp(argv[1], "finish"))
	{
		txn_finish();
		printf("txn_finish() = %s\n", strerror(errno));
	}
	else
	{
		printf("unknown action\n");
		return 1;
	}

	return 0;
}
