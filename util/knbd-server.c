#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>

#define KNBD_PORT 2492
#define BLOCK_SIZE 4096

static int readn(int fd, unsigned char * data, size_t length)
{
	int size = 0, value = read(fd, data, length);
	while(size < length)
	{
		if(value <= 0)
			return size ? size : value;
		size += value;
		value = read(fd, &data[size], length - size);
	}
	return size;
}

static void serve_loop(int bd, int client)
{
	struct stat st;
	unsigned char command;
	unsigned int number;
	unsigned short bs = BLOCK_SIZE;
	
	fstat(bd, &st);
	number = st.st_size / bs;
	number = htonl(number);
	bs = htons(bs);
	write(client, &number, sizeof(number));
	write(client, &bs, sizeof(bs));
	
	for(;;)
	{
		unsigned char buffer[BLOCK_SIZE];
		read(client, &command, 1);
		read(client, &number, sizeof(number));
		number = ntohl(number);
		if(number >= st.st_size / bs)
		{
			printf("Reset block %d\n", number);
			number = 0;
		}
		lseek(bd, number * BLOCK_SIZE, SEEK_SET);
		switch(command)
		{
			case 0:
				printf("Read block %d\n", number);
				read(bd, buffer, BLOCK_SIZE);
				write(client, buffer, BLOCK_SIZE);
				break;
			case 1:
				printf("Write block %d\n", number);
				readn(client, buffer, BLOCK_SIZE);
				write(bd, buffer, BLOCK_SIZE);
				break;
			default:
				printf("Unknown command 0x%02x!\n", command);
		}
	}
}

static int serve_client(int bd)
{
	int incoming, client, one = 1;
	socklen_t len = sizeof(struct sockaddr_in);
	struct sockaddr_in sin;
	
	sin.sin_family = AF_INET;
	sin.sin_port = htons(KNBD_PORT);
	sin.sin_addr.s_addr = INADDR_ANY;
	
	incoming = socket(PF_INET, SOCK_STREAM, 0);
	setsockopt(incoming, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	bind(incoming, (struct sockaddr *) &sin, len);
	listen(incoming, 3);
	
	client = accept(incoming, (struct sockaddr *) &sin, &len);
	close(incoming);
	serve_loop(bd, client);
	close(client);
	
	return 0;
}

int main(int argc, char * argv[])
{
	int bd;
	if(argc != 2)
	{
		printf("Usage: %s <bd>\n", argv[0]);
		return 0;
	}
	bd = open(argv[1], O_RDWR);
	while(!serve_client(bd));
	close(bd);
	return 0;
}
