#ifndef __FSTITCH_FSCORE_TYPES_H
#define __FSTITCH_FSCORE_TYPES_H

/* Provide weak reference callbacks */
#define PATCH_WEAKREF_CALLBACKS 0

typedef struct bdesc bdesc_t;
/* struct page pointers are non-NULL only in __KERNEL__ */
typedef struct page page_t;
typedef struct blockman blockman_t;

typedef struct patch patch_t;
typedef struct chweakref chweakref_t;
typedef struct patch_dlist patch_dlist_t;
typedef struct patch_pass_set patch_pass_set_t;

typedef struct chdepdesc chdepdesc_t;

#if PATCH_WEAKREF_CALLBACKS
typedef void (*patch_satisfy_callback_t)(chweakref_t * weak, patch_t * old, void * data);
#endif

struct chweakref {
	patch_t * patch;
#if PATCH_WEAKREF_CALLBACKS
	patch_satisfy_callback_t callback;
	void * callback_data;
#endif
	chweakref_t ** pprev;
	chweakref_t * next;
};

struct patch_dlist {
	patch_t * head;
	patch_t ** tail;
};

typedef struct BD BD_t;
typedef struct CFS CFS_t;
typedef struct LFS LFS_t;

#endif /* __FSTITCH_FSCORE_TYPES_H */
