/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_PATCH_UTIL_H
#define __FSTITCH_FSCORE_PATCH_UTIL_H

#include <fscore/patch.h>

/* mark a patch graph (i.e. set PATCH_MARKED) */
void patch_mark_graph(patch_t * root);

/* unmark a patch graph (i.e. clear PATCH_MARKED) */
void patch_unmark_graph(patch_t * root);

/* push all patches at this block device on a block (i.e. data) descriptor to a new block device and block */
int patch_push_down(bdesc_t * block, BD_t * current_bd, BD_t * target_bd);

/* create patches based on the diff of two data regions */
int patch_create_diff(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * olddata, const void * newdata, patch_t ** head);
int patch_create_diff_set(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * olddata, const void * newdata, patch_t ** tail, patch_pass_set_t * befores);

#endif /* __FSTITCH_FSCORE_PATCH_UTIL_H */
