#include <inc/lib.h>
#include <kfs/opgroup.h>

/* This utility is our first non-trivial use of user-space change descriptors!
 * It can run a sequence of commands and make each depend on the previous one.
 * The syntax is as follows:
 * 
 * $ depend cmd1 [args] [, cmd2 [args] [, ...]]
 * 
 * For example, suppose we wanted to be sure that a download with 'get' is
 * complete before we delete a previous version of the file. We could do:
 * 
 * $ depend /get http://example.com/file -o file.new , /rm file.old
 * 
 * Notice that the leading / is necessary because we don't have the shell
 * helping us out to find the binaries. */

void umain(int argc, const char * argv[])
{
	opgroup_id_t prev_id = 0;
	int r, start = 1, end;
	while(start < argc)
	{
		envid_t child;
		opgroup_id_t id = opgroup_create(0);
		
		if((r = id) < 0)
			goto error;
		if(prev_id)
		{
			r = opgroup_add_depend(id, prev_id);
			if(r < 0)
				goto error;
			opgroup_abandon(prev_id);
		}
		opgroup_release(id);
		r = opgroup_engage(id);
		if(r < 0)
			goto error;
		
		for(end = start + 1; end < argc; end++)
			if(!strcmp(argv[end], ","))
			{
				argv[end] = NULL;
				break;
			}
		child = spawn(argv[start], &argv[start]);
		if((r = child) < 0)
			goto error;
		opgroup_disengage(id);
		wait(child);
		
		prev_id = id;
		start = end + 1;
	}
	opgroup_abandon(prev_id);
	
	return;
	
error:
	kdprintf(STDERR_FILENO, "%s: %i\n", argv[0], r);
}
