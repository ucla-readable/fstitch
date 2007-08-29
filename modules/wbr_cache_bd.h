/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_WBR_CACHE_BD_H
#define __FSTITCH_FSCORE_WBR_CACHE_BD_H

#include <fscore/bd.h>

BD_t * wbr_cache_bd(BD_t * disk, uint32_t soft_dblocks, uint32_t soft_blocks);

#endif /* __FSTITCH_FSCORE_WBR_CACHE_BD_H */
