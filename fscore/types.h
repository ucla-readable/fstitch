/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_TYPES_H
#define __FSTITCH_FSCORE_TYPES_H

/* Provide weak reference callbacks */
#define PATCH_WEAKREF_CALLBACKS 0

typedef struct bdesc bdesc_t;
/* struct page pointers are non-NULL only in __KERNEL__ */
typedef struct page page_t;
typedef struct blockman blockman_t;

typedef struct patch patch_t;
typedef struct patchweakref patchweakref_t;
typedef struct patch_dlist patch_dlist_t;
typedef struct patch_pass_set patch_pass_set_t;

typedef struct patchdep patchdep_t;

#if PATCH_WEAKREF_CALLBACKS
typedef void (*patch_satisfy_callback_t)(patchweakref_t * weak, patch_t * old, void * data);
#endif

struct patchweakref {
	patch_t * patch;
#if PATCH_WEAKREF_CALLBACKS
	patch_satisfy_callback_t callback;
	void * callback_data;
#endif
	patchweakref_t ** pprev;
	patchweakref_t * next;
};

struct patch_dlist {
	patch_t * head;
	patch_t ** tail;
};

typedef struct BD BD_t;
typedef struct CFS CFS_t;
typedef struct LFS LFS_t;

#endif /* __FSTITCH_FSCORE_TYPES_H */
