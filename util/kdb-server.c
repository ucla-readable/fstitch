#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>

#define KFS_DEBUG_PORT 15166

#define KFS_DEBUG_MARK 0
#define KFS_DEBUG_DISABLE 1
#define KFS_DEBUG_ENABLE 2

#define KDB_MODULE_BDESC 100
#define KDB_MODULE_CHDESC_ALTER 200
#define KDB_MODULE_CHDESC_INFO 300

#define BUFFER_SIZE 256

static void print_commands(void)
{
	printf("\nCommand list:\n");
	printf("\tHELP\n");
	printf("\tMARK [module]\n");
	printf("\tDISABLE <module>\n");
	printf("\tENABLE <module>\n");
	printf("\n");
}

static void print_prompt(void)
{
	printf("-> ");
	fflush(stdout);
}

static unsigned short parse_module(char * module)
{
	if(!strcasecmp(module, "bdesc"))
		return KDB_MODULE_BDESC;
	if(!strcasecmp(module, "chdesc_alter") || !strcasecmp(module, "chdesc alter") || !strcasecmp(module, "chdesc"))
		return KDB_MODULE_CHDESC_ALTER;
	if(!strcasecmp(module, "chdesc_info") || !strcasecmp(module, "chdesc info"))
		return KDB_MODULE_CHDESC_INFO;
	printf("Unknown module.\n");
	return 0;
}

static int send_command(int fd, unsigned short * command)
{
	command[0] = htons(command[0]);
	command[1] = htons(command[1]);
	return write(fd, command, 4);
}

static void debug_loop(int log, int client)
{
	print_commands();
	print_prompt();
	
	for(;;)
	{
		char buffer[BUFFER_SIZE];
		fd_set read_set;
		
		FD_ZERO(&read_set);
		FD_SET(0, &read_set);
		FD_SET(client, &read_set);
		
		select(client + 1, &read_set, NULL, NULL, NULL);
		
		if(FD_ISSET(0, &read_set))
		{
			int bytes = read(0, buffer, sizeof(buffer) - 1);
			if(bytes > 0)
			{
				if(buffer[bytes - 1] == '\n')
					buffer[--bytes] = 0;
				else
					buffer[bytes] = 0;
				if(!strcasecmp(buffer, "help"))
					print_commands();
				else if(!strncasecmp(buffer, "mark", 4))
				{
					unsigned short command[2] = {KFS_DEBUG_MARK};
					if(!buffer[4])
					{
						command[1] = 0;
						send_command(client, command);
					}
					else if(buffer[4] == ' ')
					{
						command[1] = parse_module(&buffer[5]);
						if(command[1])
							send_command(client, command);
					}
					else
						goto error;
				}
				else if(!strncasecmp(buffer, "disable", 7))
				{
					if(!buffer[7])
						printf("DISABLE <module>\n");
					else if(buffer[7] == ' ')
					{
						unsigned short command[2] = {KFS_DEBUG_DISABLE};
						command[1] = parse_module(&buffer[8]);
						if(command[1])
							send_command(client, command);
					}
					else
						goto error;
				}
				else if(!strncasecmp(buffer, "enable", 6))
				{
					if(!buffer[6])
						printf("ENABLE <module>\n");
					else if(buffer[6] == ' ')
					{
						unsigned short command[2] = {KFS_DEBUG_ENABLE};
						command[1] = parse_module(&buffer[7]);
						if(command[1])
							send_command(client, command);
					}
					else
						goto error;
				}
				else if(buffer[0])
				    error:
					printf("Unknown command.\n");
				print_prompt();
			}
		}
		if(FD_ISSET(client, &read_set))
		{
			int bytes = read(client, buffer, sizeof(buffer));
			if(bytes > 0)
				write(log, buffer, bytes);
			else
				break;
		}
	}
	printf("\nConnection reset.\n");
}

static int debug_listen(int log)
{
	int incoming, client, one = 1;
	socklen_t len = sizeof(struct sockaddr_in);
	struct sockaddr_in sin;
	
	sin.sin_family = AF_INET;
	sin.sin_port = htons(KFS_DEBUG_PORT);
	sin.sin_addr.s_addr = INADDR_ANY;
	
	incoming = socket(PF_INET, SOCK_STREAM, 0);
	if(incoming == -1)
		return -1;
	if(setsockopt(incoming, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)))
	{
		close(incoming);
		return -1;
	}
	if(bind(incoming, (struct sockaddr *) &sin, len))
	{
		close(incoming);
		return -1;
	}
	if(listen(incoming, 3))
	{
		close(incoming);
		return -1;
	}
	
	client = accept(incoming, (struct sockaddr *) &sin, &len);
	close(incoming);
	if(client == -1)
		return -1;
	debug_loop(log, client);
	close(client);
	
	return 0;
}

int main(int argc, char * argv[])
{
	int log;
	
	if(argc != 2)
	{
		printf("Usage: %s <log>\n", argv[0]);
		return 0;
	}
	
	log = open(argv[1], O_WRONLY | O_TRUNC | O_CREAT, 0644);
	if(log == -1)
	{
		perror(argv[1]);
		return 1;
	}
	
	debug_listen(log);
	
	close(log);
	
	return 0;
}
