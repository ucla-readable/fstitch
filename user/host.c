#include <inc/lib.h>

void
print_usage(char *bin)
{
	fprintf(STDERR_FILENO, "%s: <hostname>\n", bin);
}

void
umain(int argc, char **argv)
{
	char *name;
	struct ip_addr ip;
	int r;

	if (argc != 2 || !strcmp("-h", argv[1]))
	{
		print_usage(argv[0]);
		exit();
	}

	name = argv[1];
	r = gethostbyname(name, &ip);
	if (r < 0)
		fprintf(STDERR_FILENO, "gethostbyname(): %e\n", r);
	else
		printf("%s\n", inet_iptoa(ip));
}
