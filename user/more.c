/* page stdin by an optional number of lines */

#include <inc/lib.h>

static int
atoi(char *s)
{
	int i;
	int ret = 0;

	for (i = 0;;i++) {
		if (s[i] == '\0') break;
		ret *= 10;
		ret += s[i] - '0';
	}
	return ret;
}

// not buffered, buf should hold max+1 chars (for the null)
static int
f_readline(int f, char *buf, int max)
{
	int n;
	int ret = 0;
	int i;

	for (i = 0; i < max; i++) {
		n = read(f, buf + ret, 1);
		ret += n;
		if (n <= 0) {
			buf[ret] = '\0';
			return ret;
		}
		assert(n == 1);
		if (buf[ret - 1] == '\n')
			return ret;
	}
	return ret;
}

static void
print_usage(char *bin)
{
	printf("%s [term row count [term col count] ]\n", bin);
}

void
umain(int argc, char **argv)
{
	int lines = 24;
	int cols = 80;
	char buf[cols+1];
	int n, i, r;

	if (argc > 3) {
		print_usage(argv[0]);
		return;
	}
	if (argc == 2 && strcmp(argv[1], "-h") == 0) {
		print_usage(argv[0]);
		return;
	}
	if (argc >= 2) 
		lines = atoi(argv[1]) - 1;
	if (argc == 3)
		cols = atoi(argv[2]);

	for (;;) {
		for (i = 0; i < lines; i++) {
			n = f_readline(0, buf, cols);
			if (n <= 0)
				return;
			if ((r = write(1, buf, n)) != n)
				panic("write error when copying");
		}
		printf("-- MORE --\r");
		n = f_readline(2, buf, cols);
		if (n <= 0 || buf[0] == 'q')
			return;
		/* erase the "-- MORE --" line */
		printf("\e[A\r          \r");
	}
}
