#include <inc/lib.h>

#define BUFFER_SIZE 1024

static uint8_t buffer[BUFFER_SIZE];

static uint32_t display_line(uint32_t * offset, uint32_t max)
{
	uint32_t line, index = *offset % BUFFER_SIZE;
	
	printf("%08x ", *offset);
	for(line = 0; line != 16; line++)
	{
		if(line == 8)
			printf(" ");
		if(line < max)
			printf(" %02x", buffer[index + line]);
		else
			printf("   ");
	}
	
	printf("  |");
	for(line = 0; line != 16; line++)
	{
		if(line < max)
		{
			uint8_t byte = buffer[index + line];
			if(byte < ' ' || '~' < byte)
				byte = '.';
			printf("%c", byte);
		}
		else
			printf(" ");
	}
	printf("|\n");
	
	line = MIN(16, max);
	*offset += line;
	return line;
}

void umain(int argc, char * argv[])
{
	uint32_t offset = 0, size;
	uint32_t limit = 0;
	int fd;
	
	if(argc != 2 && argc != 4)
	{
usage:
		kdprintf(STDERR_FILENO, "Usage: %s [--limit limit[kM]] file\n", argv[0]);
		return;
	}
	
	if(argc == 4)
	{
		uint32_t multiplier = 1;
		int last = strlen(argv[2]) - 1;
		char * end = NULL;
		
		if(strcmp(argv[1], "--limit"))
			goto usage;
		
		switch(argv[2][last])
		{
			case 'M':
				multiplier *= 1024;
			case 'k':
				multiplier *= 1024;
				argv[2][last--] = 0;
		}
		
		if(!argv[2][0])
			goto usage;
		limit = strtol(argv[2], &end, 0);
		if(*end)
			goto usage;
		limit *= multiplier;
	}
	
	fd = open(argv[argc - 1], O_RDONLY);
	if(fd < 0)
	{
		kdprintf(STDERR_FILENO, "%s: %e\n", argv[argc - 1], fd);
		return;
	}
	
	size = read(fd, buffer, BUFFER_SIZE);
	while(size > 0)
	{
		uint32_t index = 0;
		if(limit && offset + size > limit)
			size = limit - offset;
		while(index < size)
			index += display_line(&offset, size - index);
		if(limit && offset == limit)
			break;
		size = read(fd, buffer, BUFFER_SIZE);
	}
	
	close(fd);
}
