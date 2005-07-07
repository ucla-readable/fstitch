#include <inc/lib.h>
#include <inc/sb16.h>

static int next_block(int fd, int block)
{
	void * target = SB16_USER_BUFFER + (block ? SB16_USER_BLOCK_SIZE : 0);
	int size;
	
	size = read(fd, target, SB16_USER_BLOCK_SIZE);
	if(size <= 0)
	{
		memset(target, 0, SB16_USER_BLOCK_SIZE);
		return -1;
	}
	
	if(size < SB16_USER_BLOCK_SIZE)
		memset(target + size, 0, SB16_USER_BLOCK_SIZE - size);
	
	return 0;
}

int pipe_play(char * name)
{
	int error, fd = 0, block;
	
	if(iscons(fd))
	{
		printf("%s: will not read audio from terminal.\n", name);
		return -1;
	}
	
	/* open the sound device */
	error = sys_sb16_open(44100, 1, (uintptr_t) SB16_USER_BUFFER);
	if(error)
	{
		printf("sys_sb16_open: %e\n", error);
		return error;
	}
	
	/* prime the sound buffer */
	next_block(fd, 0);
	
	printf("sys_sb16_start() = %e\n", sys_sb16_start());
	block = sys_sb16_wait();
	
	/* loop until the sound is done */
	error = next_block(fd, block);
	while(!error)
	{
		block = sys_sb16_wait();
		error = next_block(fd, block);
	}
	
	return 0;
}

int file_play(char * prefix)
{
	int error, i, fd[10], number, block;
	char filename[MAXNAMELEN];
	
	/* open the sound device */
	error = sys_sb16_open(44100, 1, (uintptr_t) SB16_USER_BUFFER);
	if(error)
	{
		printf("sys_sb16_open: %e\n", error);
		return error;
	}
	
	/* give ourselves nice high priority */
	sys_env_set_priority(0, ENV_MAX_PRIORITY - 1);
	
	printf("Loading files... ");
	for(number = 0; number != 10; number++)
	{
		snprintf(filename, MAXNAMELEN, "%s.%d", prefix, number);
		fd[number] = open(filename, O_RDONLY);
		if(fd[number] < 0)
			break;
	}
	printf("done.\n");
	
	if(!number)
	{
		printf("%s: %e\n", prefix, fd[0]);
		return fd[0];
	}
	
	/* prime the sound buffer */
	next_block(fd[0], 0);
	
	printf("sys_sb16_start() = %e\n", sys_sb16_start());
	block = sys_sb16_wait();
	
	/* loop until the sound is done */
	for(i = 0; i != number; i++)
	{
		error = next_block(fd[i], block);
		while(!error)
		{
			block = sys_sb16_wait();
			error = next_block(fd[i], block);
		}
	}
	
	return 0;
}

void umain(int argc, char * argv[])
{
	int error;
	
	if(argc != 2)
	{
		printf("Usage: %s <prefix>\n", argv[0]);
		return;
	}
	
	if(strcmp(argv[1], "-"))
		error = file_play(argv[1]);
	else
		error = pipe_play(argv[0]);
	
	if(!error)
	{
		/* wait for the last block to finish */
		sys_sb16_wait();
		
		/* stop playback */
		sys_sb16_stop();
		sys_sb16_close();
	}
}
