#ifndef __KUDOS_KFS_IDE_PIO_BD_H
#define __KUDOS_KFS_IDE_PIO_BD_H

#include <lib/types.h>
#include <kfs/bd.h>

BD_t * ide_pio_bd(uint8_t controller, uint8_t disk, uint8_t readahead);

#endif /* __KUDOS_KFS_IDE_PIO_BD_H */
