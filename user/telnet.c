//
// telnet - A basic telnet client

// Documents I've found helpful:
// - A readable, and somewhat in depth, overview:
//   http://www.scit.wlv.ac.uk/~jphb/comms/telnet.html
// - Telnet's first rfc:
//   http://www.faqs.org/rfcs/rfc854.html
// - Links to all telnet rfcs:
//   http://www.omnifarious.org/~hopper/technical/telnet-rfc.html

// TODO:
// - We ignore all telnet options in telnet_recv(), implement support for these
// as helpful.
// - We don't worry about ascii control codes, implement as helpful.

#include <inc/lib.h>


static bool display_cmds  = 0; // print telnet cmds (doesn't print options, for now)


static struct telnet_state {
	int     net;
	bool    reached_eof;

	uint8_t cmd_str[2];
	int     in_telnet_cmd;
	int     in_telnet_cmd_param;
} gts;

/*---------------------------------------------------------------------------*/
static void
close_conn_and_exit(struct telnet_state *ts)
{
	exit(0);
}
/*---------------------------------------------------------------------------*/
static int
telnet_poll_send(struct telnet_state *ts)
{
	char buf[128];
	int n;
	int r;

	//
	// If user has closed all their programs, close the connection:

	if (ts->reached_eof)
		close_conn_and_exit(ts);

	//
	// Otherwise, the shell is still alive:
	// Send any data from the shell's stdout to the telnet client:

	n = read_nb(STDIN_FILENO, buf, sizeof(buf));

	if (n == -1)
	{
		return 0;
	}
	else if (n == 0)
	{
		ts->reached_eof = 1;
		return 0;
	}
	else if (n > 0)
	{
		if ((r = write(STDOUT_FILENO, buf, n)) < 0)
			panic("write: %e", r);
		if ((r = write(ts->net, buf, n)) < 0)
			panic("write: %e", r);
		if (r != n)
			panic("r (%d) != n (%d)", r, n);
		return n;
	}
	else
	{
		panic("read_nb: %e\n", n);
	}
}

// Telnet commands
#define IAC  255
#define SB   250
#define SE   240

static int
telnet_poll_recv(struct telnet_state *ts)
{
	const int telnet_cmd_len = 3;
	uint8_t c;
	int n = 0;
	int r;
	  
	// Print each character to the shell's stdin.
	// We must also watch for, and not pass on to the shell, any telnet cmds.
	while ((r = read_nb(ts->net, &c, 1)) > 0)
	{
		n++;

		if (!ts->in_telnet_cmd)
		{
			if (IAC != c)
				printf("%c", c);
			else
				ts->in_telnet_cmd = 1;
		}
		else
		{
			if (!ts->in_telnet_cmd_param)
			{
				ts->in_telnet_cmd++;
				if (SB != c)
				{
					if (ts->in_telnet_cmd == telnet_cmd_len)
					{
						ts->cmd_str[1] = c;
						if (display_cmds)
							printf("telnet cmd: %d %d\n", ts->cmd_str[0], ts->cmd_str[1]);
						ts->in_telnet_cmd = 0;
					}
					else
					{
						ts->cmd_str[0] = c;
					}
				}
				else
				{
					ts->in_telnet_cmd_param = 1;
				}
			}
			else
			{
				if (ts->in_telnet_cmd_param == 2) // IAC was previous byte
				{
					if (SE == c)
					{
						ts->in_telnet_cmd = 0;
						ts->in_telnet_cmd_param = 0;
					}
					else
					{
						ts->in_telnet_cmd_param--;
					}
				}
				else
				{
					if(IAC == c)
						ts->in_telnet_cmd_param++;
					else
						{ } // ignore data for the option
				}
			}
		}
		
	}

	if (r == 0)
		close_conn_and_exit(ts);
	
	return n;
}

static struct telnet_state*
telnet_connect(struct ip_addr addr, uint16_t port)
{
	struct telnet_state *ts;
	int r;

	ts = &gts;
	/* Initialize the structure. */
	ts->reached_eof = 0;
	ts->in_telnet_cmd = 0;
	ts->in_telnet_cmd_param = 0;

	printf("Connecting to %s:%d... ", kinet_iptoa(addr), port);
	if ((r = kconnect(addr, port, &ts->net)) < 0)
	{
		kdprintf(STDERR_FILENO, "connect: %e\n", r);
		exit(0);
	}
	printf("Connected.\n");
	
	return ts;
}

static void
telnet_poll(struct telnet_state *ts)
{
	int r_recv, r_send;

	while (1)
	{
		if ((r_recv = telnet_poll_recv(ts)) < 0)
		{
			kdprintf(STDERR_FILENO, "telnet_poll_recv: %e\n", r_recv);
			close_conn_and_exit(ts);
		}
		if ((r_send = telnet_poll_send(ts)) < 0)
		{
			kdprintf(STDERR_FILENO, "telnet_poll_send: %e\n", r_send);
			close_conn_and_exit(ts);
		}

		if (!r_recv && !r_send)
			sys_yield();
	}
}

/*---------------------------------------------------------------------------*/
static void
print_usage(char *bin)
{
	printf("%s: <host> <port>\n", bin);
	printf("Options:\n");
	printf("  -c: display telnet commands\n");
}

void
umain(int argc, char **argv)
{
	struct telnet_state *ts;
	int argv_i = 1;
	int r;

	if (argc < 3 || (argc >=2 && !strcmp("-h", argv[1])))
	{
		print_usage(argv[0]);
		exit(0);
	}

	display_cmds = get_arg_idx(argc, (const char**) argv, "-c");

	printf("Telnet Client\n");

	struct ip_addr connect_ipaddr;
	r = kgethostbyname(argv[argv_i++], &connect_ipaddr);
	if (r < 0)
	{
		kdprintf(STDERR_FILENO, "Bad ip address string \"%s\": %e\n", argv[argv_i-1], r);
		exit(0);
	}

	u16_t port = strtol(argv[argv_i++], NULL, 10);

	ts = telnet_connect(connect_ipaddr, port);
	if (!ts)
	{
		kdprintf(STDERR_FILENO, "telnet_connect returned NULL\n");
		exit(0);
	}

	telnet_poll(ts); // won't return
}
