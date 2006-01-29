#include <inc/lib.h>

/* a general purpose pseudorandom number generator */
int rand(int nseed)
{
	static int seed = 0;
	if(nseed)
		seed = nseed;
	seed *= 214013;
	seed += 2531011;
	return (seed >> 16) & 0x7fff;
}

/* buffer space for demos */
uint8_t demo_buffer[5 * 64000] = {0};

void data(int argc, char * argv[]);
void fall(int argc, char * argv[]);
void fire(int argc, char * argv[]);
void ladybug(int argc, char * argv[]);
void life(int argc, char * argv[]);
void matrix(int argc, char * argv[]);
void pong(int argc, char * argv[]);
void swirl(int argc, char * argv[]);
void tv(int argc, char * argv[]);
void wars(int argc, char * argv[]);

static const struct {
	const char * name;
	void (*demo)(int, char *[]);
} demos[] = {
	{"data", data},
	{"fall", fall},
	{"fire", fire},
	{"ladybug", ladybug},
	{"life", life},
	{"matrix", matrix},
	{"pong", pong},
	{"swirl", swirl},
	{"tv", tv},
	{"wars", wars}
};

#define DEMO_COUNT (sizeof(demos) / sizeof(demos[0]))

void umain(int argc, char * argv[])
{
	int i;
	
	if((argc < 2) ||
	   !strcmp(argv[1], "-h") ||
	   !strcmp(argv[1], "--help"))
	{
		printf("Usage: %s <demo>\n", argv[0]);
		
		printf("Where demo can be one of:");
		for(i = 0; i != DEMO_COUNT; i++)
			printf(" %s", demos[i].name);
		printf("\n");
		
		return;
	}
	
	for(i = 0; i != DEMO_COUNT; i++)
		if(!strcmp(argv[1], demos[i].name))
		{
			demos[i].demo(argc - 1, &argv[1]);
			return;
		}
	
	printf("No such demo: %s\n", argv[1]);
}
