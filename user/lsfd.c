#include <inc/lib.h>

static void
usage(void)
{
	printf("usage: lsfd [-1|-2]\n");
	exit();
}

void
umain(int argc, char **argv)
{
	int i, usefd = -1;
	struct Stat st;

	ARGBEGIN{
	default:
		usage();
	case '1':
		usefd = 1;
		break;
	case '2':
		usefd = 2;
		break;
	}ARGEND

	for (i=0; i<32; i++)
		if (fstat(i, &st) >= 0) {
			if (usefd != -1)
				kdprintf(usefd, "fd %d: name %s isdir %d size %d dev %s\n",
					i, st.st_name, st.st_isdir,
					st.st_size, st.st_dev->dev_name);	
			else
				printf_c("fd %d: name %s isdir %d size %d dev %s\n",
					i, st.st_name, st.st_isdir,
					st.st_size, st.st_dev->dev_name);
		}
}
