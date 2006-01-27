#include <string.h>
#include <inc/lib.h>
#include <arch/simple.h>
/* for byte order translations */
#include <ipv4/lwip/inet.h>

#define KNBD_PORT 2492
#define BLOCK_SIZE 4096

static struct knbd_state {
	struct ip_addr remote_ip;
	int16_t remote_port;
	int net;
	int bd;
} gks;
typedef struct knbd_state knbd_state_t;

static bool display_conns = 1;
static bool display_reqs  = 0;

static uint8_t buffer[BLOCK_SIZE];

static void knbd_serve(knbd_state_t * ks)
{
	struct Stat st;
	unsigned char command;
	unsigned int number;
	unsigned short bs = BLOCK_SIZE;
	
	fstat(ks->bd, &st);
	number = st.st_size / bs;
	number = htonl(number);
	bs = htons(bs);
	write(ks->net, &number, sizeof(number));
	write(ks->net, &bs, sizeof(bs));
	
	for(;;)
	{
		if(read(ks->net, &command, 1) != 1)
			break;
		if(read(ks->net, &number, sizeof(number)) != 4)
			break;
		number = ntohl(number);
		if(number >= st.st_size / bs)
		{
			kdprintf(STDERR_FILENO, "knbdd: Reset block %u\n", number);
			number = 0;
		}
		seek(ks->bd, number * BLOCK_SIZE);
		switch(command)
		{
			case 0:
				if (display_reqs)
					printf("knbdd: Read block %u\n", number);
				read(ks->bd, buffer, BLOCK_SIZE);
				write(ks->net, buffer, BLOCK_SIZE);
				break;
			case 1:
				if (display_reqs)
					printf("knbdd: Write block %u\n", number);
				readn(ks->net, buffer, BLOCK_SIZE);
				write(ks->bd, buffer, BLOCK_SIZE);
				break;
			default:
				kdprintf(STDERR_FILENO, "knbdd: Unknown command 0x%02x!\n", command);
		}
	}
}

static int knbd_accept(const char * bd_filename, int fd, struct ip_addr remote_ip, uint16_t remote_port)
{
	struct knbd_state * ks = &gks;
	int bd;

	bd = open(bd_filename, O_RDWR);
	if (bd < 0)
	{
		kdprintf(STDERR_FILENO, "knbdd %s(%s): open: %e\n", __FUNCTION__, bd_filename, bd);
		return bd;
	}
	
	// Initialize ks
	ks->remote_ip = remote_ip;
	ks->remote_port = remote_port;
	ks->net = fd;
	ks->bd = bd;
	
	if (display_conns)
		printf("knbdd connection accepted from %s:%u\n",
				 inet_iptoa(ks->remote_ip),
				 ks->remote_port);

	knbd_serve(ks);
	close(bd);

	if (display_conns)
		printf("knbdd connection closed   with %s:%u\n",
				 inet_iptoa(ks->remote_ip),
				 ks->remote_port);

	return 0;
}

static int knbd_listen(const char * bd_filename, uint16_t port)
{
	uint32_t listen_key;
	int fd;
	struct ip_addr remote_ip;
	uint16_t remote_port;
	int r;

	if ((r = bind_listen(ip_addr_any, port, &listen_key)) < 0)
	{
		kdprintf(STDERR_FILENO, "knbdd: bind_listen: %e\n", r);
		exit(0);
	}

	// Accept connections and fork to handle each connection
	while (1)
	{
		if ((r = accept(listen_key, &fd, &remote_ip, &remote_port)) < 0)
		{
			kdprintf(STDERR_FILENO, "knbdd accept: %e\n", r);
			exit(0);
		}

		if ((r = fork()) < 0)
		{
			kdprintf(STDERR_FILENO, "knbdd fork: %e\n", r);
			exit(0);
		}
		if (r == 0)
		{
			knbd_accept(bd_filename, fd, remote_ip, remote_port);
			exit(0);
		}

		if ((r = close(fd)) < 0)
		{
			kdprintf(STDERR_FILENO, "knbdd close: %e\n", r);
			exit(0);
		}
	}
}

int umain(int argc, const char * argv[])
{
	const char * port_str;
	int16_t port = KNBD_PORT;

	if(argc != 2)
	{
		printf("Usage: %s <bd_file> [-p port] [-c] [-r]\n", argv[0]);
		return 0;
	}

	if ((port_str = get_arg_val(argc, argv, "-p")))
		port = strtol(port_str, NULL, 10);

	if (get_arg_idx(argc, argv, "-c"))
		display_conns = 1;
	if (get_arg_idx(argc, argv, "-r"))
		display_reqs = 1;

	knbd_listen(argv[1], port);

	return 0;
}
