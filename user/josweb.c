//
// JOSWeb - A Webserver for KudOS
// Features file serving from the filesystem, 404s, basic cgi,
// and basic http header support.
//
// Based on Adam Dunkels' <adam@sics.se> httpd.
// Changes: Corrections for the KudOS environment, real fs access rather
// than static strings, cgi support, basic http header support
// added, and ported from the lwip raw interface to jos' netd.

// Documents I've found helpful:
// - http://www.freesoft.org/CIE/RFC/1945/index.htm
// - Simple grammar and error codes:
//   http://www.cs.ucl.ac.uk/staff/jon/book/node168.html
// - Basics of HTTP/1.0 and 1.1 for clients and servers:
//   http://www.jmarshall.com/easy/http/
// - Basics of HTTP/1.0:
//   http://www.freesoft.org/CIE/Topics/102.htm
// - HTTP/1.1 rfc:
//   ftp://ftp.isi.edu/in-notes/rfc2616.txt
// - CGI:
//   http://hoohoo.ncsa.uiuc.edu/cgi/

// TODO:
// - Port /server/stop from lwip-josweb to netd-josweb, kill the accept()
//   josweb.


/*
 * Copyright (c) 2001-2003 Swedish Institute of Computer Science.
 * All rights reserved. 
 * 
 * Redistribution and use in source and binary forms, with or without modification, 
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED 
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT 
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, 
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT 
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING 
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILIlibTY 
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 * 
 * Author: Adam Dunkels <adam@sics.se>
 *
 */

#include <inc/malloc.h>
#include <inc/lib.h>


static bool display_conns = 0;


struct httpd_state_data {
	char  *data;
	size_t len;
	int    fd; // -1 if no associated fd
};

static struct httpd_state {
	struct ip_addr remote_ip;
	uint16_t remote_port;
	int net[2];
	struct httpd_state_data file;
} ghs;


/*---------------------------------------------------------------------------*/
static void
close_conn_and_exit(struct httpd_state *hs)
{
	int r;

	if (hs)
	{
		if (hs->file.fd != -1)
			if((r = close(hs->file.fd)) < 0)
				kdprintf(STDERR_FILENO, "WARNING: httpd: close: %e", r);
		if ((r = close(hs->net[0])) < 0)
			kdprintf(STDERR_FILENO, "WARNING: httpd: close: %e\n", r);
		if ((r = close(hs->net[1])) < 0)
			kdprintf(STDERR_FILENO, "WARNING: httpd: close: %e\n", r);
	}

	exit();
}
/*---------------------------------------------------------------------------*/

//
//
// FS code

struct fs_file {
	char *data;
	int   fd;
	u32_t len;
};

static char cgi_output[128*1024];
static const char* cgi_bin = "/cgi-bin/";
static const char* server_bin = "/server/";

static int run_cgi(char *bin_name, struct fs_file *file);
static int server(char *bin_name, struct fs_file *file);

static int
fs_open(char *filename, struct fs_file *file)
{
	int r;
	int fd;

	// Pass cgi-bin reqs to run_cgi()
	if (!strncmp(filename, cgi_bin, strlen(cgi_bin))
		 && strlen(filename) > strlen(cgi_bin))
		return run_cgi(filename, file);

	// Pass server reqs to server()
	if (!strncmp(filename, server_bin, strlen(server_bin))
		 && strlen(filename) > strlen(server_bin))
		return server(filename, file);


	if ((fd = open(filename, O_RDONLY)) < 0)
	{
		if (fd == -E_NOT_FOUND)
			return 0;
		else
			panic("open '%s': %e", filename, fd);		
	}

	struct Stat stat;
	if ((r = fstat(fd, &stat)) < 0)
		panic("fstat: %e");

	struct Fd *fds;
	if ((r = fd_lookup(fd, &fds)) < 0)
		panic("fd_lookup: %e", r);

	file->fd   = fd;
	file->data = NULL;
	file->len  = stat.st_size;

	// fd is closed during connection close

	return 1;
}


// Run the function display_fn() and send its output to file.
// NOTE: this is implemented by fork()ing and running display_fn() in the
// fork child.
static int
fd_display(void (*display_fn)(void *), void *arg, struct fs_file *file)
{
	int fork_child;
	int p[2];
	int r;

	if ((r = pipe(p)) < 0)
	{
		kdprintf(STDERR_FILENO, "pipe(): %e\n", r);
		exit();
	}
	
	if ((r = fork()) < 0)
	{
		kdprintf(STDERR_FILENO, "fork(): %e\n", r);
		exit();
	}
	if (r == 0)
	{
		if ((r = dup2(p[1], STDOUT_FILENO)) < 0)
		{
			kdprintf(STDERR_FILENO, "dup2(): %e\n", r);
			exit();
		}
		if ((r = dup2(STDOUT_FILENO, STDERR_FILENO)) < 0)
		{
			kdprintf(STDERR_FILENO, "dup2(): %e\n", r);
			exit();
		}
		
		if ((r = close(p[0])) < 0)
		{
			kdprintf(STDERR_FILENO, "close(): %e\n", r);
			exit();
		}
		if ((r = close(p[1])) < 0)
		{
			kdprintf(STDERR_FILENO, "close(): %e\n", r);
			exit();
		}
		
		display_fn(arg);
		
		close_all();
		exit();
		return 0; // appease compiler, *shouldn't* get here
	}
	else
	{
		fork_child = r;
		
		if ((r = close(p[1])) < 0)
		{
			kdprintf(STDERR_FILENO, "close(): %e\n", r);
			exit();
		}
		
		// read fork child's output from p[0]
		
		int n = 0; // index of next byte in cgi_output
		
		do
		{
			unsigned char c;
			r = read(p[0], &c, 1);
			
			if (r > 0)
			{
				if(n >= sizeof(cgi_output))
					panic("n >= sizeof(cgi_output)");
				cgi_output[n++] = c;
			}
			else if (r < 0)
			{
				panic("read: %e", r);
			}
			else if (r == 0)
			{
			}
		} while (r != 0);
		
		if ((r = close(p[0])) < 0)
		{
			kdprintf(STDERR_FILENO, "close(): %e\n", r);
			exit();
		}
		
		file->data = cgi_output;
		file->fd   = -1;
		file->len  = n;
		
		wait(fork_child);
		return 1;
	}
}

//
// CGI support

static int
parse_argv(char *bin_name, char **argv, size_t argv_len)
{
	int i = 0;
	int first = 1;
	const char cgi_isindex = '?';
	const char cgi_space   = '+';

	char *c = bin_name;

	assert(argv_len >= 2); // at least the bin_name and null-term

	// argv[0] is the binary's name (stripped of the preceeding '/' it seems?)
	argv[i++] = ++c;

	// if the isindex '?' is given, parse each '+'-seperated argument
	while (i < argv_len-1)
	{
		if (first)
		{
			c = strchr(c, cgi_isindex);
			first = 0;
		}
		else
		{
			c = strchr(c, cgi_space);
		}
		if (c == 0)
			break;

		*(c++) = 0; // null-term prev string
		argv[i++] = c;
	}

	argv[i++] = 0; // null-term argv

	return 0;
}

static void
spawn_cgi(void *arg)
{
	int r;
	int spawn_child=0;
	char *bin_name = (char*) arg;

#define MAXARGS 16 // Defined same as in sh.c, might as well.
	char *argv[MAXARGS];
	argv[MAXARGS-1] = 0;
	if ((r = parse_argv(bin_name, argv, MAXARGS)) < 0)
	{
		kdprintf(STDERR_FILENO, "parse_argv(): %e", r);
		exit();
	}
	if ((r = spawn(bin_name, (const char**) argv)) < 0)
	{
		kdprintf(STDERR_FILENO, "spawn(): %e", r);
		exit();
	}
	spawn_child = r;
	
	close_all();
	wait(spawn_child);
}

static int
run_cgi(char *bin_name, struct fs_file *file)
{
	// Strip "/cgi-bin" from the filename
	bin_name = &bin_name[strlen(cgi_bin) - 1];

	return fd_display(spawn_cgi, bin_name, file);
}


//
// Server support

static void
stats_display_noarg(void *arg)
{
	printf("<html><body>stats has moved to <a href=\"/cgi-bin/netstats\">/cgi-bin/netstats</a>.</body></html>");
}

static int
server(char *request, struct fs_file *file)
{
	// Strip "/server/" from the filename
	request = &request[strlen(server_bin)];

	if (!strcmp("stats", request))
	{
		return fd_display(stats_display_noarg, NULL, file);
	}
	else if (!strcmp("stop", request))
	{
		// its ok to not have httpd_state, since we'll exit:
		kdprintf(STDERR_FILENO, "josweb /server/stop not yet ported from lwip to netd-based josweb\n");
		close_conn_and_exit(NULL);
		return 0; // appease compiler
	}
	else
	{
		kdprintf(1, "Unknown server request for \"%s\"\n", request);
		return 0;
	}
}

/*---------------------------------------------------------------------------*/
//
//
// httpd

static void
send_http_header(struct httpd_state *hs, int http_status)
{
	const char *status_str;
	int r;

	//
	// Status

	if (200 == http_status)
		status_str = "OK";
	else if (404 == http_status)
		status_str = "Not Found";
	else if (500 == http_status)
		status_str = "Server Error";
	else
		panic("Unimplemented http status code %d", http_status);

	if ((r = kdprintf(hs->net[1], "HTTP/1.0 %d %s\r\n", http_status, status_str)) < 0)
		panic("kdprintf: %e");

	//
	// Server
	
	if ((r = kdprintf(hs->net[1], "Server: JOSWeb/1.0\r\n")) < 0)
		panic("kdprintf: %e", r);

	//
	// Content length

	if ((r = kdprintf(hs->net[1], "Content-Length: %d\r\n", hs->file.len)) < 0)
		panic("kdprintf: %e", r);

	//
	// Connection

	if ((r = kdprintf(hs->net[1], "Connection: close\r\n")) < 0)
		panic("kdprintf: %e", r);

	//
	// End of header

	if ((r = kdprintf(hs->net[1], "\r\n")) < 0)
		panic("kdprintf: %e", r);
}


static char request_pg[PGSIZE];

static int
httpd_serve(struct httpd_state *hs)
{
	struct fs_file file;
	int http_status = 200;
	int send_header = 1;
	char *request = request_pg;
	int i = 0;
	int r;

	// Read "GET <URI> [<HTTP VERSION>]\r\n"
	while (1)
	{
		if ((r = read(hs->net[0], &request[i], 1)) < 0)
		{
			kdprintf(STDERR_FILENO, "read: %e\n", r);
			close_conn_and_exit(hs);
		}
		if (request[i] == '\n' || i >= sizeof(request_pg)-1)
			break;
		i++;
	} 
	request[i] = 0;

	// Process the request
	if (strncmp(request, "GET", 3) == 0)
	{
		char *c;

		request += strlen("GET")+1;

		//
		// Parse out the resource
		char *resource;
		c = strchr(request, '/');
		if (!c)
		{
			kdprintf(STDERR_FILENO, "malformed request '%s'\n", request);
			exit();
		}
		resource = request = c;

		//
		// See if there is an "HTTP/x"
		// If so, include an http header in our response
		c = strchr(request, 'H');
		if (c && !strncmp(c, "HTTP/", strlen("HTTP/")))
			send_header = 1;
		else
			send_header = 0;

		//
		// Null-term end of the resource
		for (i = 0; i < 40; i++)
		{
			if ((resource[i] == ' ')
				 || (resource[i] == '\r')
				 || (resource[i] == '\n')) {
				resource[i] = 0;
			}
		}
		if (display_conns)
			printf("Serving GET for \"%s\"\n", resource);

		//
		// Use resource
		if (*(resource + 1) == 0)
		{
			if (!fs_open("/index.html", &file))
				panic("Unable to open /index.html");
		}
		else if (!fs_open(resource, &file))
		{
			kdprintf(STDERR_FILENO, "Unable to open \"%s\", returning 404\n", resource);
			http_status = 404;
			if (!fs_open("/404.html", &file))
				panic("Unable to open /404.html");
		}
		hs->file.data = file.data;
		hs->file.fd   = file.fd;
		hs->file.len  = file.len;

		// Disregard the rest of the request
	}
	else
	{
		// Don't support this request type or is a malformed request
		kdprintf(STDERR_FILENO, "Unsupported request: \"%s\"\n", request);

		hs->file.data = "";
		hs->file.len  = 0;

		http_status = 500;
		send_header = 1;
	}

	if (send_header)
		send_http_header(hs, http_status);
 
	if (hs->file.fd >= 0)
	{
		uint8_t * buf;
		int nbytes;

		buf = malloc(PGSIZE);
		if (!buf)
		{
			kdprintf(STDERR_FILENO, "%s:%d malloc failed\n", __FILE__, __LINE__);
			close_conn_and_exit(hs);
		}

		for (i=0; i < hs->file.len; i += PGSIZE)
		{
			r = nbytes = read(hs->file.fd, buf, PGSIZE);
			if (r < 0)
			{
				kdprintf(STDERR_FILENO, "%s:%d read: %e\n", __FILE__, __LINE__, r);
				free(buf);
				close_conn_and_exit(hs);
			}

			r = write(hs->net[1], buf, nbytes);
			if (r != nbytes)
			{
				kdprintf(STDERR_FILENO, "%s:%d write: %e\n", __FILE__, __LINE__, r);
				free(buf);
				close_conn_and_exit(hs);
			}
		}

		free(buf);
	}
	else
	{
		if ((r = write(hs->net[1], hs->file.data, hs->file.len)) < 0)
		{
			kdprintf(STDERR_FILENO, "write: %e\n", r);
			close_conn_and_exit(hs);
		}
	}

	return 0;
}

static int
httpd_accept(int fd[2], struct ip_addr remote_ip, uint16_t remote_port)
{
	struct httpd_state *hs = &ghs;
	int r;
	
	// Initialize hs
	hs->remote_ip = remote_ip;
	hs->remote_port = remote_port;
	hs->net[0] = fd[0];
	hs->net[1] = fd[1];
	hs->file.data = NULL;
	hs->file.fd   = -1;
	
	if (display_conns)
		printf("http connection accepted from %s:%d\n",
				 inet_iptoa(hs->remote_ip),
				 hs->remote_port);

	if ((r = httpd_serve(hs)) < 0)
		return r;

	if (display_conns)
		printf("http connection closed   with %s:%d\n",
				 inet_iptoa(hs->remote_ip),
				 hs->remote_port);

	return 0;
}

static void
httpd_listen(void)
{
	uint32_t listen_key;
	int fd[2];
	struct ip_addr remote_ip;
	uint16_t remote_port;
	int r;

	if ((r = bind_listen(ip_addr_any, 80, &listen_key)) < 0)
	{
		kdprintf(STDERR_FILENO, "bind_listen: %e\n", r);
		exit();
	}

	// Accept connections and fork to handle each connection
	while (1)
	{
		if ((r = accept(listen_key, fd, &remote_ip, &remote_port)) < 0)
		{
			kdprintf(STDERR_FILENO, "accept: %e\n", r);
			exit();
		}

		if ((r = fork()) < 0)
		{
			kdprintf(STDERR_FILENO, "fork: %e\n", r);
			exit();
		}
		if (r == 0)
		{
			if ((r = httpd_accept(fd, remote_ip, remote_port)) < 0)
			{
				kdprintf(STDERR_FILENO, "httpd_accept: %e\n", r);
				exit();
			}
			exit();
		}

		if ((r = close(fd[0])) < 0)
		{
			kdprintf(STDERR_FILENO, "close: %e\n", r);
			exit();
		}
		if ((r = close(fd[1])) < 0)
		{
			kdprintf(STDERR_FILENO, "close: %e\n", r);
			exit();
		}
	}
}

/*---------------------------------------------------------------------------*/
static void
print_usage(char *bin)
{
	printf("%s\n", bin);
	printf("Options:\n");
	printf("  -q: turn off connected/disconnected output to stdout\n");
}

void
umain(int argc, char **argv)
{
	if (argc == 0)
	{
		binaryname = "josweb";
		sys_env_set_name(0, "josweb");
	}
	if (argc >= 2 && !strcmp("-h", argv[1]))
	{
		print_usage(argv[0]);
		exit();
	}

	display_conns = !get_arg_idx(argc, (const char**) argv, "-q");

	if (display_conns)
		printf("JOSWeb Server\n");

	httpd_listen();
}
