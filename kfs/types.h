#ifndef __KUDOS_KFS_TYPES_H
#define __KUDOS_KFS_TYPES_H

/* Provide weak reference callbacks */
#define CHDESC_WEAKREF_CALLBACKS 0

typedef struct bdesc bdesc_t;
/* struct page pointers are non-NULL only in __KERNEL__ */
typedef struct page page_t;
typedef struct blockman blockman_t;

typedef struct chdesc chdesc_t;
typedef struct chweakref chweakref_t;
typedef struct chdesc_dlist chdesc_dlist_t;
typedef struct chdesc_pass_set chdesc_pass_set_t;

typedef struct chdepdesc chdepdesc_t;

#if CHDESC_WEAKREF_CALLBACKS
typedef void (*chdesc_satisfy_callback_t)(chweakref_t * weak, chdesc_t * old, void * data);
#endif

struct chweakref {
	chdesc_t * chdesc;
#if CHDESC_WEAKREF_CALLBACKS
	chdesc_satisfy_callback_t callback;
	void * callback_data;
#endif
	chweakref_t ** pprev;
	chweakref_t * next;
};

struct chdesc_dlist {
	chdesc_t * head;
	chdesc_t ** tail;
};

typedef struct BD BD_t;
typedef struct CFS CFS_t;
typedef struct LFS LFS_t;

#endif /* __KUDOS_KFS_TYPES_H */
