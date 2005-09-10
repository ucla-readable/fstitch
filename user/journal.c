#include <kfs/journal_bd.h>
#include <kfs/block_resizer_bd.h>
#include <kfs/wb_cache_bd.h>
#include <kfs/modman.h>

#include <inc/cfs_ipc_client.h>
#include <inc/kfs_ipc_client.h>
#include <inc/kfs_uses.h>
#include <arch/simple.h>
#include <inc/stdio.h>

static BD_t * find_bd(const char * name)
{
	BD_t * c;
	modman_it_t it;
	int r = modman_it_init_bd(&it);
	if(r < 0)
		panic("modman_it_init_bd() failed: %e\n", r);
	
	while((c = modman_it_next_bd(&it)))
	{
		const char * scan = modman_name_bd(c);
		if(scan && !strcmp(scan, name))
			break;
	}
	
	modman_it_destroy(&it);
	
	if(!c)
		printf("No such device: %s\n", name);
	
	return c;
}

void umain(int argc, const char ** argv)
{
	if(argc < 3 || argc > 5)
	{
		printf("Usage:\n");
		printf("%s start <journal_bd> [$]<journal> [new_size]\n", argv[0]);
		printf("%s stop <journal_bd>\n", argv[0]);
	}
	else if((argc == 4 || argc == 5) && !strcmp(argv[1], "start"))
	{
		BD_t * journal_bd = find_bd(argv[2]);
		if(journal_bd)
		{
			BD_t * journal;
			bool use_cache = 0;
			if(argv[3][0] == '$')
			{
				use_cache = 1;
				argv[3]++;
			}
			
			journal = find_bd(argv[3]);
			if(journal)
			{
				if(argc == 5)
				{
					BD_t * resizer = NULL;
					uint16_t new_size = strtol(argv[4], NULL, 0);
					if(new_size <= 0)
						printf("Invalid size: %s\n", new_size);
					else
						resizer = block_resizer_bd(journal, new_size);
					DESTROY_LOCAL(journal);
					journal = resizer;
				}
				
				if(journal)
				{
					BD_t * cache = use_cache ? wb_cache_bd(journal, 128) : journal;
					if(cache)
					{
						int r = journal_bd_set_journal(journal_bd, cache);
						if(r < 0)
						{
							DESTROY(cache);
							if(argc == 5)
								DESTROY(journal);
							printf("%e\n", r);
						}
					}
					else
						printf("Could not create cache!\n");
				}
				else
					printf("Could not create block resizer!\n");
			}
		}
	}
	else if(argc == 3 && !strcmp(argv[1], "stop"))
	{
		BD_t * journal_bd = find_bd(argv[2]);
		if(journal_bd)
		{
			int r = journal_bd_set_journal(journal_bd, NULL);
			if(r < 0)
				printf("%e\n", r);
		}
	}
	else
		printf("Invalid options.\n");
}