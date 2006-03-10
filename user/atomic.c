#include <inc/lib.h>
#include <kfs/opgroup.h>

/* This utility runs a sequence of commands and atomically commits their
 * changes to disk using atomic opgroups.
 * The syntax is as follows:
 * 
 * $ atomic cmd1 [args] [, cmd2 [args] [, ...]]
 * 
 * For example, suppose we wanted to be sure that we add an email to both
 * the new/ directory and all/ directory and do not add the email to just
 * one of the directories. We could do:
 * 
 * $ atomic cp foo_mail new/foo_mail , cp foo_mail old/foo_mail
 *
 * We could ensure the source foo_mail is not deleted until the email is
 * added to the mail directories:
 * (note that neither depend nor atomic support escaped ',', so one can not
 * actually do this yet)
 *
 * $ depend atomic cp foo_mail new/foo_mail \, cp foo_mail old/foo_mail , rm foo_mail
 * */
void umain(int argc, const char * argv[])
{
	int r, start = 1, end;
	opgroup_id_t id = opgroup_create(OPGROUP_FLAG_ATOMIC);
	const char * call = NULL;

	if((r = id) < 0)
	{
		call = "opgroup_create";
		goto error;
	}

	r = opgroup_engage(id);
	if(r < 0)
	{
		call = "opgroup_engage";
		goto error;
	}

	while(start < argc)
	{
		envid_t child;
		for(end = start + 1; end < argc; end++)
			if(!strcmp(argv[end], ","))
			{
				argv[end] = NULL;
				break;
			}
		child = spawn(argv[start], &argv[start]);
		if((r = child) < 0)
		{
			call = "spawn";
			goto error;
		}
		wait(child);
		start = end + 1;
	}

	if((r = opgroup_disengage(id)) < 0)
	{
		call = "opgroup_disengage";
		goto error;
	}
	if((r = opgroup_release(id)) < 0)
	{
		call = "opgroup_release";
		goto error;
	}
	if((r = opgroup_abandon(id)) < 0)
	{
		call = "opgroup_abandon";
		goto error;
	}
	
	return;
	
error:
	kdprintf(STDERR_FILENO, "%s: %s: %i\n", argv[0], call, r);
}
