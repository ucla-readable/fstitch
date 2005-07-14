#include <stdio.h>

int main(int argc, char *argv[])
{
	char tc[3];
	unsigned int tn;
	FILE * in = stdin;
	
	if(argc > 1)
	{
		in = fopen(argv[1], "r");
		if(!in)
		{
			perror(argv[1]);
			return 1;
		}
	}
	
	fscanf(in, " %2s", tc);
	while(!feof(in))
	{
		sscanf(tc, "%X", &tn);
		printf("%c", (char) tn);
		
		fscanf(in, " %2s", tc);
	}
	
	return 0;
}
