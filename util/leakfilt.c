#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This small program parses the output of using the diagnostic malloc code in
 * lib/malloc.c and displays a summary of the memory allocated and freed. The
 * strings --malloc0-- and --malloc1-- have special significance when in the
 * output, and are necessary for anything useful to happen. Ask Mike how to use
 * this utility if it is not clear from the code below. */

struct allocation {
	int number;
	int size;
	void * addr;
	void * allocator;
	void * freer;
	struct allocation * next;
};

static int allocation_number = 0;
static struct allocation * allocations = NULL;

static void push_malloc(int size, void * addr, void * caller)
{
	struct allocation * a = malloc(sizeof(*a));
	a->number = allocation_number++;
	a->size = size;
	a->addr = addr;
	a->allocator = caller;
	a->freer = NULL;
	a->next = allocations;
	allocations = a;
}

static void clear_malloc(void * addr, void * caller)
{
	struct allocation * scan = allocations;
	while(scan)
	{
		if(scan->addr == addr && !scan->freer)
		{
			scan->freer = caller;
			break;
		}
		scan = scan->next;
	}
}

static void process_malloc(char * line)
{
	int size;
	void * addr;
	void * caller;
	int i = sscanf(line, "malloc(%d) = %p, from %p", &size, &addr, &caller);
	if(i == 3)
		push_malloc(size, addr, caller);
}

static void process_free(char * line)
{
	void * addr;
	void * caller;
	int i = sscanf(line, "free(%p), from %p", &addr, &caller);
	if(i == 2)
		clear_malloc(addr, caller);
}

static void process_line(char * line)
{
	static int record_malloc = 0;
	char * sub = strstr(line, "malloc(");
	if(sub && record_malloc)
		process_malloc(sub);
	sub = strstr(line, "free(");
	if(sub)
		process_free(sub);
	sub = strstr(line, "--malloc0--");
	if(sub)
		record_malloc = 0;
	sub = strstr(line, "--malloc1--");
	if(sub)
		record_malloc = 1;
}

static void _display_leaks(int all, struct allocation * scan)
{
	if(scan)
	{
		_display_leaks(all, scan->next);
		if(all)
			printf("#%d, %p: size %d, allocated by %p, freed by %p\n", scan->number, scan->addr, scan->size, scan->allocator, scan->freer);
		else if(!scan->freer)
			printf("#%d, %p: size %d, allocated by %p\n", scan->number, scan->addr, scan->size, scan->allocator);
	}
}

static void display_leaks(int all)
{
	_display_leaks(all, allocations);
}

int main(int argc, char * argv[])
{
	char line[256];
	int all = 0;
	
	if(argc > 1)
		all = !strcmp(argv[1], "--all");
	
	fgets(line, sizeof(line), stdin);
	while(!feof(stdin))
	{
		process_line(line);
		fgets(line, sizeof(line), stdin);
	}
	
	display_leaks(all);
	
	return 0;
}
