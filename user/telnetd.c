//
// telnetd - A basic telnet server

// Documents I've found helpful:
// - A readable, and somewhat in depth, overview:
//   http://www.scit.wlv.ac.uk/~jphb/comms/telnet.html
// - Telnet's first rfc:
//   http://www.faqs.org/rfcs/rfc854.html
// - Links to all telnet rfcs:
//   http://www.omnifarious.org/~hopper/technical/telnet-rfc.html

// TODO:
// - Much of our existing code, even when it knows to use fds for io,
// generally uses printf() for status and error information. This is often
// not what we want for the case of a remote shell, the shell user should
// see such output.
// - We ignore all telnet options in telnet_recv(), implement support for these
// as helpful.
// - We don't worry about ascii control codes, implement as helpful.

#include <inc/lib.h>


bool display_conns = 0;
bool display_cmds  = 0; // print telnet cmds (doesn't print options, for now)

struct telnetd_state {
	struct ip_addr remote_ip;
	uint16_t remote_port;

	int  net[2];
	int  stdin;
	int  stdout;
	bool reached_eof;

	uint8_t cmd_str[2];
	int in_telnet_cmd;
	int in_telnet_cmd_param;

	envid_t fork_child;
} gts;

/*---------------------------------------------------------------------------*/
static void
close_conn_and_exit(struct telnetd_state *ts)
{
	int r;

	if (!ts->reached_eof)
	{
		// The client started the close connection.
		// In this case, we destroy their shell

		// TODO: destroy the children of ts->fork_child, too.
		// Chris' env_destroy(envid, 1) does this.

		if ((r = sys_env_destroy(ts->fork_child)) < 0)
			fprintf(STDERR_FILENO, "WARNING: telnetd: sys_env_destroy: %e\n", r);
	}

	if ((r = close(ts->net[0])) < 0)
		fprintf(STDERR_FILENO, "WARNING: telnetd: close: %e\n", r);
	if ((r = close(ts->net[1])) < 0)
		fprintf(STDERR_FILENO, "WARNING: telnetd: close: %e\n", r);
	if ((r = close(ts->stdin)) < 0)
		fprintf(STDERR_FILENO, "WARNING: telnetd: close: %e\n", r);
	if ((r = close(ts->stdout)) < 0)
		fprintf(STDERR_FILENO, "WARNING: telnetd: close: %e\n", r);

	if (display_conns)
		printf("telnet connection closed   with %s:%d\n",
				 inet_iptoa(ts->remote_ip),
				 ts->remote_port);

	exit();
}
/*---------------------------------------------------------------------------*/
static int
telnetd_poll_send(struct telnetd_state *ts)
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

	n = read_nb(ts->stdout, buf, sizeof(buf));
	  
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
		if ((r = write(ts->net[1], buf, n)) < 0)
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
telnetd_poll_recv(struct telnetd_state *ts)
{
	const int telnet_cmd_len = 3;
	uint8_t c;
	int n = 0;
	int r;
	
	// Print each character to the shell's stdin.
	// We must also watch for, and not pass on to the shell, any telnet cmds.
	while ((r = read_nb(ts->net[0], &c, 1)) > 0)
	{
		n++;
		
		if (!ts->in_telnet_cmd)
		{
			if (IAC != c)
				fprintf(ts->stdin, "%c", c);
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
					if (IAC == c)
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

static void
telnetd_poll(struct telnetd_state *ts)
{
	int r_recv, r_send;

	while (1)
	{
		if ((r_recv = telnetd_poll_recv(ts)) < 0)
		{
			fprintf(STDERR_FILENO, "telnetd_poll_recv: %e\n", r_recv);
			close_conn_and_exit(ts);
		}
		if ((r_send = telnetd_poll_send(ts)) < 0)
		{
			fprintf(STDERR_FILENO, "telnetd_poll_send: %e\n", r_send);
			close_conn_and_exit(ts);
		}

		if (!r_recv && !r_send)
			sys_yield();
	}
}
/*---------------------------------------------------------------------------*/
static void
telnetd_accept(int fd[2], struct ip_addr remote_ip, uint16_t remote_port)
{
	struct telnetd_state *ts = &gts;
	
	// Initialize ts
	ts->remote_ip = remote_ip;
	ts->remote_port = remote_port;
	ts->net[0] = fd[0];
	ts->net[1] = fd[1];
	ts->stdin = -1;
	ts->stdout = -1;
	ts->reached_eof = 0;
	ts->in_telnet_cmd = 0;
	ts->in_telnet_cmd_param = 0;
	ts->fork_child = 0;

	//
	// Setup fds and spawn the shell and begin busing data inbetween
	// the network connection and the shell's fds.
	
	int spawn_child=0;
	int stdin[2], stdout[2];
	int r;

  	if ((r = pipe(stdin)) < 0)
	{
		fprintf(STDERR_FILENO, "pipe(): %e\n", r);
		exit();
	}
  	if ((r = pipe(stdout)) < 0)
	{
		fprintf(STDERR_FILENO, "pipe(): %e\n", r);
		exit();
	}
	ts->stdin  = stdin[1];
	ts->stdout = stdout[0];

	if ((r = fork()) < 0)
	{
		fprintf(STDERR_FILENO, "fork(): %e\n", r);
		exit();
	}
	if (r == 0)
	{
		// Close the network fds, child doesn't get them
		if ((r = close(fd[0])) < 0)
		{
			fprintf(STDERR_FILENO, "close(%d): %e\n", stdin[0], r);
			exit();
		}
		if ((r = close(fd[1])) < 0)
		{
			fprintf(STDERR_FILENO, "close(%d): %e\n", stdin[0], r);
			exit();
		}

		// Setup std fds
		if ((r = dup2(stdin[0], STDIN_FILENO)) < 0)
		{
			fprintf(STDERR_FILENO, "dup2(%d, 0): %e\n", stdin[0], r);
			exit();
		}
		if ((r = dup2(stdout[1], STDOUT_FILENO)) < 0)
		{
			fprintf(STDERR_FILENO, "dup2(%d, 1): %e\n", stdout[1], r);
			exit();
		}
		if ((r = dup2(STDOUT_FILENO, STDERR_FILENO)) < 0)
		{
			fprintf(STDERR_FILENO, "dup2(%d, 1): %e\n", STDOUT_FILENO, r);
			exit();
		}

		// Close original telnetd<->sh fds
		if ((r = close(stdin[0])) < 0)
		{
			fprintf(STDERR_FILENO, "close(%d): %e\n", stdin[0], r);
			exit();
		}
		if ((r = close(stdin[1])) < 0)
		{
			fprintf(STDERR_FILENO, "close(%d): %e\n", stdin[1], r);
			exit();
		}
		if ((r = close(stdout[0])) < 0)
		{
			fprintf(STDERR_FILENO, "close(%d): %e\n", stdout[0], r);
			exit();
		}
		if ((r = close(stdout[1])) < 0)
		{
			fprintf(STDERR_FILENO, "close(%d): %e\n", stdout[1], r);
			exit();
		}
		// Note: past this point we have only print_c to error to.

		if ((r = spawnl("/sh", "sh", "-i", (char*) 0)) < 0)
		{
			printf_c("telnetd: spawn sh: %e\n", r);
			exit();
		}
		spawn_child = r;

		close_all();
		wait(spawn_child);
		exit();
	}
	else
	{
		// As fork parent, transfer incoming net data to child's stdin
		// and child's stdout to outgoing net data, until the telnetd<->child
		// fds are in use by no one but us, meaning the child has closed them.

		ts->fork_child = r;

		if ((r = close(stdin[0])) < 0)
		{
			fprintf(STDERR_FILENO, "close(%d): %e\n", stdin[0], r);
			exit();
		}
		if ((r = close(stdout[1])) < 0)
		{
			fprintf(STDERR_FILENO, "close(%d): %e\n", stdout[1], r);
			exit();
		}

		if (display_conns)
			printf("telnet connection accepted from %s:%d\n",
					 inet_iptoa(ts->remote_ip),
					 ts->remote_port);

		telnetd_poll(ts); // won't return
	}
}
/*---------------------------------------------------------------------------*/
void
telnetd_listen(void)
{
	uint32_t listen_key;
	int fd[2];
	struct ip_addr remote_ip;
	uint16_t remote_port;
	int r;

	if ((r = bind_listen(ip_addr_any, 23, &listen_key)) < 0)
	{
		fprintf(STDERR_FILENO, "bind_listen: %e\n", r);
		exit();
	}

	// Accept connections and fork to handle each connection
	while (1)
	{
		if ((r = accept(listen_key, fd, &remote_ip, &remote_port)) < 0)
		{
			fprintf(STDERR_FILENO, "accept: %e\n", r);
			exit();
		}

		if ((r = fork()) < 0)
		{
			fprintf(STDERR_FILENO, "fork: %e\n", r);
			exit();
		}
		if (r == 0)
		{
			telnetd_accept(fd, remote_ip, remote_port); // won't return
		}

		if ((r = close(fd[0])) < 0)
		{
			fprintf(STDERR_FILENO, "close: %e\n", r);
			exit();
		}
		if ((r = close(fd[1])) < 0)
		{
			fprintf(STDERR_FILENO, "close: %e\n", r);
			exit();
		}
	}
}

/*---------------------------------------------------------------------------*/
void
print_usage(char *bin)
{
	printf("%s\n", bin);
	printf("Options:\n");
	printf("  -q: turn off connected/disconnected output to stdout\n");
	printf("  -c: display telnet commands\n");
}

void
umain(int argc, char **argv)
{
	if (argc == 0)
	{
		binaryname = "telnetd";
		sys_env_set_name(0, "telnetd");
	}
	if (argc >= 2 && !strcmp("-h", argv[1]))
	{
		print_usage(argv[0]);
		exit();
	}

	display_conns = !get_arg_idx(argc, (const char**) argv, "-q");
	display_cmds  = get_arg_idx(argc, (const char**) argv, "-c");

	if (display_conns)
		printf("Telnet Server\n");

	telnetd_listen();
}
