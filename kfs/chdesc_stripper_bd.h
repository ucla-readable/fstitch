#ifndef __KUDOS_KFS_CHDESC_STRIPPER_BD_H
#define __KUDOS_KFS_CHDESC_STRIPPER_BD_H

// Strip blocks' inter-BD dependecies away as they are written to
// chdesc_stripper_bd.
//
// This module is useful when someone further up is creating chdescs
// but there is no cache, or other module satisfying dependencies, below the
// chdesc creator. chdesc_stripper_bd preserves inter-BD dependency
// requirements.

#include <inc/types.h>
#include <kfs/bd.h>

BD_t * chdesc_stripper_bd(BD_t * disk);

#endif /* __KUDOS_KFS_CHDESC_STRIPPER_BD_H */
