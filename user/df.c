#include <inc/lib.h>

// TODO:
// - handle overflow in our unit conversions

void
print_usage(char *bin)
{
	printf("%s: [-kmp] [file...]\n", bin);
}

int
convert_unit(int num, int scale)
{
	int i;
	if (scale > 0) {
		num *= BLKSIZE;
		for (i = 0; i < scale; i++)
			num /= 1024;
	}
	return num;
}

void
umain(int argc, char **argv)
{
#if !ENABLE_ENV_FP || 1 // 1 until printf("%x.yf") supported
	typedef uint32_t avail_t;
	const char *print_str = "%d%s\n";
#else
	typedef double   avail_t;
	const char *print_str = "%.1f%s\n";
#endif

	avail_t reported = 0;
	int  scale = 1;
	char   * reported_unit = "K";
	int i, avail;

	ARGBEGIN{
	default:
		print_usage(argv[0]);
		exit();
	case 'k':
		scale = 1;
		reported_unit = "K";
		break;
	case 'm':
		scale = 2;
		reported_unit = "M";
		break;
	case 'p':
		scale = 0;
		reported_unit = " pages";
		break;
	}ARGEND
	
	if (argc == 0) {
		avail = disk_avail_space("/");
		if (avail < 0) {
			printf("%s: %e\n", "/", avail);
		}
		else {
			reported = convert_unit(avail, scale);
			printf(print_str, reported, reported_unit);
		}
	}
	else {
		for (i = 0; i < argc; i++) {
			avail = disk_avail_space(argv[i]);
			if (avail < 0) {
				printf("%s: %e\n", argv[i], avail);
			}
			else {
				reported = convert_unit(avail, scale);
				printf(print_str, reported, reported_unit);
			}
		}
	}
}
