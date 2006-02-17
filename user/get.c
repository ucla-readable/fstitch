//
// get - A network downloader.

// Documents I've found helpful:
// - josweb.c
// - telnet.c

// TODO:
// - When saving to file and saving header, take header into account when
//   checking file size against http header length field. (Search for FIXME,
//   too.)

#include <inc/lib.h>
#include <inc/malloc.h>


static int   fileout_fd           = STDOUT_FILENO;
static const char *fileout_name   = NULL;
static int   status_fd            = STDOUT_FILENO;
static int   silent               = 0;
static int   print_server_headers = 0; // print the headers sent by the server
static int   save_server_headers  = 0; // save  the headers sent by the server


static struct http_state {
	int net;

	uint8_t buf[PGSIZE];

	char    cur_header_line[320]; // 320 is a long line, "good enough".
	char   *cur_header_line_c;
	size_t  header_end_sofar;
	size_t  body_length;
	size_t  body_sofar;
	size_t  body_sofar_period;
	size_t  body_sofar_shown;
} ghs;

/*---------------------------------------------------------------------------*/
static void
close_conn(struct http_state *hs)
{
	if (!silent && hs->body_length)
		kdprintf(status_fd, "\n");

	// Confirm we recved all data:
	if (hs->body_length)
	{
		if (hs->body_sofar != hs->body_length)
			kdprintf(STDERR_FILENO,
					  "http header said %d bytes, but we recved %d.\n",
					  hs->body_length, hs->body_sofar);

		if (fileout_name)
		{
			struct Stat stat;
			int r;
			if((r = fstat(fileout_fd, &stat)) < 0)
				panic("stat: %i", r);
			if(stat.st_size != hs->body_length)
				kdprintf(STDERR_FILENO,
						  "http header said %d bytes, but our file is %d.\n",
						  hs->body_length, stat.st_size);
		}
	}

	exit(0);
}

static void
removeoutput_close_exit(struct http_state *hs)
{
	if (!silent)
		kdprintf(status_fd, "Exiting\n");
	if (fileout_name)
	{
		int r;
		if ((r = close(fileout_fd)) < 0)
			kdprintf(STDERR_FILENO, "WARNING (ignoring): %i\n", r);
		if ((r = remove(fileout_name)) < 0)
			kdprintf(STDERR_FILENO, "WARNING (ignoring): %i\n", r);
	}

	exit(0);
}
/*---------------------------------------------------------------------------*/
static void
init_body_length_settings(struct http_state *hs)
{
	hs->body_sofar_period = ROUNDUP32(hs->body_length, 80) / 80;
	if (hs->body_length && !silent && !print_server_headers)
		kdprintf(status_fd, "Size: %d bytes\n", hs->body_length);
/*
	if (fileout_name && hs->body_length > MAXFILESIZE)
	{
		// FIXME: this size check does not check size of header!
		if (!silent)
			kdprintf(STDERR_FILENO,
					  "Requested file too large for our filesystem, exiting\n");
		removeoutput_close_exit(hs);
	}
*/
}

static void
update_body_length_display(struct http_state *hs)
{
	if (!hs->body_length)
		return;

	while (hs->body_sofar_shown < hs->body_sofar * 80 / hs->body_length)
	{
		if (!silent && fileout_fd != status_fd)
			kdprintf(status_fd, "=");
		hs->body_sofar_shown++;
	}
}

/*---------------------------------------------------------------------------*/
// http header keys
static const char hk_http[]   = "HTTP";
static const char hk_length[] = "Content-Length: ";

static int
http_read_header(struct http_state *hs)
{
	char c;
	int r;

	while ((r = read(hs->net, &c, 1)))
	{
		if (print_server_headers)
			kdprintf(status_fd, "%c", c);
		if (save_server_headers && fileout_fd != status_fd)
			kdprintf(fileout_fd, "%c", c);
		
		if (c == '\n' || c == '\r')
		{
			// Check the header line
			if (!strncmp(hk_http, hs->cur_header_line, strlen(hk_http)))
			{
				// request status
				char *code_str = &hs->cur_header_line[strlen("HTTP/1.0 ")];
				char *code_str_end = strchr(code_str, ' ');
				//char *msg = code_str_end + 1;
				
				if (!silent)
					kdprintf(status_fd, "%s\n", code_str);
				
				*code_str_end = 0;
				const long code = strtol(code_str, NULL, 10);
				if (200 != code)
				{
					// It might be good to not error on all cases, eg 302
					removeoutput_close_exit(hs);
				}
			}
			else if (!strncmp(hk_length, hs->cur_header_line, strlen(hk_length)))
			{
				// content length 
				char *body_length_str = hs->cur_header_line + strlen(hk_length);
				hs->body_length = (size_t) strtol(body_length_str, NULL, 10);
				init_body_length_settings(hs);
			}
			else
			{
				// Ignored header line
			}
			
			// Continue on
			hs->header_end_sofar++;
			hs->cur_header_line_c = hs->cur_header_line;
			memset(hs->cur_header_line, 0, sizeof(hs->cur_header_line));
		}
		else
		{
			hs->header_end_sofar = 0;
			*(hs->cur_header_line_c++) = c;
		}
			
		if (hs->header_end_sofar == 4)
		{
			if(!hs->body_length && !silent && !print_server_headers)
				kdprintf(status_fd, "Size: unknown\n");
			return 0;
		}
	}

	kdprintf(STDERR_FILENO, "Connection closed while reading http header\n");
	return -1;
}

static int
http_read_body(struct http_state *hs)
{
	int r;

	while ((r = read(hs->net, hs->buf, sizeof(hs->buf))))
	{
		const int read_len = r;
		r = write(fileout_fd, hs->buf, read_len);
		if (r != read_len)
			panic("r != read_len");
		hs->body_sofar += r;
		if (fileout_fd != status_fd)
			update_body_length_display(hs);
	}

	return 0;
}

static void
http_get(struct ip_addr addr, uint16_t port, const char *uri, const char *host)
{
	struct http_state *hs = NULL;
	int r;

	if (!silent)
		kdprintf(status_fd, "http target: addr = %s, port = %d, resource = \"%s\"\n",
		        kinet_iptoa(addr), port, uri);

	hs = &ghs;
	// Initialize hs
	hs->header_end_sofar = 0;
	hs->cur_header_line_c = hs->cur_header_line;
	hs->body_length = 0;
	hs->body_sofar = 0;
	hs->body_sofar_period = 0;
	hs->body_sofar_shown = 0;

	// Connect
	if (!silent)
		kdprintf(status_fd, "Connecting... ");
	if ((r = kconnect(addr, port, &hs->net)) < 0)
	{
		kdprintf(STDERR_FILENO, "connect: %i\n", r);
		exit(0);
	}
	if (!silent)
		kdprintf(status_fd, "Connected\n");

	// Send the request
	kdprintf(hs->net, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", uri, host);
	if (!silent)
		kdprintf(status_fd, "Sending request... ");

	// Read response
	if ((r = http_read_header(hs)) < 0)
	{
		kdprintf(STDERR_FILENO, "http_read_header: %i\n", r);
		removeoutput_close_exit(hs);
	}
	if ((r = http_read_body(hs)) < 0)
	{
		kdprintf(STDERR_FILENO, "http_read_body: %i\n", r);
		removeoutput_close_exit(hs);
	}

	close_conn(hs);
}
/*---------------------------------------------------------------------------*/
static const char root[] = "/";
static const char http[] = "http://";

static int
parse_url(char *url, struct ip_addr *addr, u16_t *port, char **resource, char **host)
{
	char  addr_str[256]; // 255 is max length of a host name, +1 for '\0'
	char  port_str[6]; // 5 is max length of a 16bit port, +1 for '\0'
	char *url_end;
	char *addr_in_url;
	char *addr_in_url_end;
	char *port_in_url;
	char *port_in_url_end;
	char *resource_in_url;
	int r;

	if(!strncmp(http, url, strlen(http)))
		url += strlen(http);

	addr_in_url     = url;
	port_in_url     = strchr(addr_in_url, ':');
	resource_in_url = strchr(addr_in_url, '/');

	url_end = url + strlen(url);//strchr(url, '\0');
	if (port_in_url)
		addr_in_url_end = port_in_url;
	else if(resource_in_url)
		addr_in_url_end = resource_in_url;
	else
		addr_in_url_end = url_end;
	if (resource_in_url)
		port_in_url_end = resource_in_url;
	else
		port_in_url_end = url_end;

	//
	// Parse address

	memcpy(addr_str, addr_in_url, MIN(addr_in_url_end - addr_in_url, sizeof(addr_str)));
	addr_str[MIN(addr_in_url_end - addr_in_url, sizeof(addr_str))] = 0;
	if (addr_in_url_end - addr_in_url > sizeof(addr_str))
	{
		kdprintf(STDERR_FILENO, "ip address string too long: \"%s\"\n", addr_str);
		return -1;
	}

	r = kgethostbyname(addr_str, addr);
	if (r < 0)
	{
		kdprintf(STDERR_FILENO, "Bad ip address string \"%s\": %i\n", addr_str, r);
		return -1;
	}

	//
	// Parse port

	if (port_in_url)
	{
		port_in_url++; // skip over the ':'
		memcpy(port_str, port_in_url, MIN(port_in_url_end - port_in_url, sizeof(port_str)));
		port_str[MIN(port_in_url_end - port_in_url, sizeof(port_str))] = 0;
		if(port_in_url_end - port_in_url > sizeof(port_str)) {
			kdprintf(STDERR_FILENO, "port string too long: \"%s\"\n", port_str);
			return -1;
		}

		*port = (u16_t) strtol(port_str, NULL, 10);
	}
	else
		*port = 80;

	//
	// Parse resource

	if (resource_in_url)
		*resource = resource_in_url;
	else
		*resource = (char*) root;

	//
	// Copy host string

	*host = strdup(addr_str);

	return 0;
}

/*---------------------------------------------------------------------------*/
static void
print_usage(char *bin)
{
	kdprintf(STDERR_FILENO, "%s: [http://]<host>[:<port>][<resource>] [OPTIONS]\n", bin);
	kdprintf(STDERR_FILENO, "Options:\n");
	kdprintf(STDERR_FILENO, "  -o <file>: save to file\n");
	kdprintf(STDERR_FILENO, "  -q: turn off status output\n");
	kdprintf(STDERR_FILENO, "  -e: redirect status output to stderr\n");
	kdprintf(STDERR_FILENO, "  -S: print server headers\n");
	kdprintf(STDERR_FILENO, "  -s: save  server headers\n");
}

void
umain(int argc, char **argv)
{
	int r;

	if (argc < 2 || (argc >=2 && !strcmp("-h", argv[1])))
	{
		print_usage(argv[0]);
		exit(0);
	}
	
	const char *filename = get_arg_val(argc, (const char**) argv, "-o");
	if (filename)
	{
		if ((r = open(filename, O_WRONLY|O_CREAT|O_TRUNC)) < 0)
		{
			kdprintf(STDERR_FILENO, "open: %i\n", r);
			exit(0);
		}
		fileout_fd   = r;
		fileout_name = filename;
	}
	else
		fileout_fd = STDOUT_FILENO;

	silent               = get_arg_idx(argc, (const char**) argv, "-q");
	print_server_headers = get_arg_idx(argc, (const char**) argv, "-S");
	save_server_headers  = get_arg_idx(argc, (const char**) argv, "-s");
	if (get_arg_idx(argc, (const char**) argv, "-e"))
		status_fd = STDERR_FILENO;
	else
		status_fd = STDOUT_FILENO;

	char *url = argv[1];
	struct ip_addr addr;
	u16_t port;
	char *uri = NULL;
	char *host = NULL;
	if ((r = parse_url(url, &addr, &port, &uri, &host)) < 0)
	{
		kdprintf(STDERR_FILENO, "parse_url: %i\n", r);
		exit(0);
	}

	http_get(addr, port, uri, host);
	free(host);
}
