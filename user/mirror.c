#include <kfs/ide_pio_bd.h>
#include <kfs/nbd_bd.h>
#include <kfs/loop_bd.h>
#include <kfs/block_resizer_bd.h>
#include <kfs/wt_cache_bd.h>
#include <kfs/wb_cache_bd.h>
#include <kfs/chdesc_stripper_bd.h>
#include <kfs/journal_queue_bd.h>
#include <kfs/wholedisk_lfs.h>
#include <kfs/mirror_bd.h>
#include <kfs/josfs_base.h>
#include <kfs/journal_lfs.h>
#include <kfs/uhfs.h>
#include <kfs/modman.h>

#include <inc/cfs_ipc_client.h>
#include <inc/kfs_ipc_client.h>
#include <inc/kfs_uses.h>
#include <arch/simple.h>
#include <inc/stdio.h>

void print_usage(const char * bin)
{
	printf("Usage:\n");
	printf("%s add mirror_bd disk controller diskno\n", bin);
	printf("%s add mirror_bd net ip port\n", bin);
	printf("%s remove mirror_bd diskno\n", bin);
	exit();
}

void umain(int argc, const char ** argv)
{
	BD_t * disk = NULL;
	BD_t * mirror = NULL;

	if (argc < 4 || argc > 6 || argc == 5)
		print_usage(argv[0]);

	{
		modman_it_t * it;
		BD_t * c;

		it = modman_it_create_bd();
		if (!it)
		{
			panic("modman_it_create_bd() failed\n");
		}

		while ((c = modman_it_next_bd(it)))
		{
			const char * name = modman_name_bd(c);
			if (name && !strcmp(name, argv[2])) {
				mirror = c;
				break;
			}
		}
	}

	if (argc == 4 && strcmp(argv[1], "remove") == 0) {
		int diskno = strtol(argv[3], NULL, 10);
		if (mirror)
			mirror_bd_remove_device(mirror, diskno);
	}
	else if (argc == 6 && strcmp(argv[1], "add") == 0) {
		if (!mirror)
			return;

		if (!strcmp(argv[3], "net")) {
			int port = strtol(argv[5], NULL, 10);
			disk = nbd_bd(argv[4], port);
		}
		else if (!strcmp(argv[3], "disk")) {
			int controller = strtol(argv[4], NULL, 10);
			int diskno = strtol(argv[5], NULL, 10);
			disk = ide_pio_bd(controller, diskno);
		}
	}

	if (!disk)
		return;

	if (mirror != NULL)
		mirror_bd_add_device(mirror, disk);
}

