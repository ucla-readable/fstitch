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
	printf("%s create disk <controller> <diskno> <stride>\n", bin);
	printf("%s create net <ip> <port> <stride>\n", bin);
	printf("%s create bd <bd_name> <stride>\n", bin);
	printf("%s add <mirror_bd> disk <controller> <diskno>\n", bin);
	printf("%s add <mirror_bd> net <ip> <port>\n", bin);
	printf("%s add <mirror_bd> bd <bd_name>\n", bin);
	printf("%s remove mirror_bd <diskno>\n", bin);
	exit();
}

BD_t * find_bd(const char * mirror_name)
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
		if (name && !strcmp(name, mirror_name))
			return c;
	}

	return NULL;
}

void umain(int argc, const char ** argv)
{
	BD_t * disk = NULL;
	BD_t * mirror = NULL;

	if (argc < 4 || argc > 6)
		print_usage(argv[0]);

	if (argc == 4 && strcmp(argv[1], "remove") == 0) {
		mirror = find_bd(argv[2]);
		int diskno = strtol(argv[3], NULL, 10);
		if (mirror)
			mirror_bd_remove_device(mirror, diskno);
	}
	else if (argc == 6 && strcmp(argv[1], "add") == 0) {
		mirror = find_bd(argv[2]);
		if (!mirror)
			exit();

		if (!strcmp(argv[3], "net")) {
			int port = strtol(argv[5], NULL, 10);
			disk = nbd_bd(argv[4], port);
		}
		else if (!strcmp(argv[3], "disk")) {
			int controller = strtol(argv[4], NULL, 10);
			int diskno = strtol(argv[5], NULL, 10);
			disk = ide_pio_bd(controller, diskno);
		}

		if (!disk)
			exit();

		mirror_bd_add_device(mirror, disk);
	}
	else if (argc == 5 && strcmp(argv[1], "add") == 0) {
		mirror = find_bd(argv[2]);
		if (!mirror)
			exit();
		if (!strcmp(argv[3], "bd"))
			disk = find_bd(argv[4]);

		if (!disk)
			exit();

		mirror_bd_add_device(mirror, disk);
	}
	else if (argc == 6 && strcmp(argv[1], "create") == 0) {
		if (!strcmp(argv[2], "net")) {
			int port = strtol(argv[4], NULL, 10);
			disk = nbd_bd(argv[3], port);
		}
		else if (!strcmp(argv[2], "disk")) {
			int controller = strtol(argv[3], NULL, 10);
			int diskno = strtol(argv[4], NULL, 10);
			disk = ide_pio_bd(controller, diskno);
		}

		int stride = strtol(argv[5], NULL, 10);
		if (disk) {
			printf("Creating mirror device\n");
			mirror = mirror_bd(disk, disk, stride); // disk0 == disk1 -> disk1 = null
		}

		if (mirror)
			printf("Mirror created\n");
	}
	else if (argc == 5 && strcmp(argv[1], "create") == 0) {
		if (!strcmp(argv[2], "bd"))
			disk = find_bd(argv[3]);
		int stride = strtol(argv[4], NULL, 10);
		if (disk) {
			printf("Creating mirror device\n");
			mirror = mirror_bd(disk, disk, stride); // disk0 == disk1 -> disk1 = null
		}

		if (mirror)
			printf("Mirror created\n");
	}

	return;
}

