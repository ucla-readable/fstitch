#include <inc/lib.h>

void umain(int argc, char * argv[])
{
	char * regex = "";
	char * line;
	
	if(argc > 1)
		regex = argv[1];
	
	line = readline(NULL);
	while(line)
	{
		if(strstr(line, regex))
			printf("%s\n", line);
		line = readline(NULL);
	}
}
