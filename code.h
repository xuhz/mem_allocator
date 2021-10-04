#ifndef _MYALLOC_H
#define _MYALLOC_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	LIST_INSERT(prev, curr, type)				\
{								\
	if (type == HEAP) {					\
		mcb_t *next = (prev)->heap_next;		\
		(curr)->heap_next = (next);			\
		(curr)->heap_prev = (prev);			\
		(prev)->heap_next = (curr);			\
		(next)->heap_prev = (curr);			\
	} else {						\
		mcb_t *next = (prev)->free_next;		\
		(curr)->free_next = (next);			\
		(curr)->free_prev = (prev);			\
		(prev)->free_next = (curr);			\
		(next)->free_prev = (curr);			\
	}							\
}

#define	LIST_REMOVE(curr, type)					\
{								\
	if (type == HEAP) {					\
		mcb_t *prev = (curr)->heap_prev;		\
		mcb_t *next = (curr)->heap_next;		\
		(prev)->heap_next = (next);			\
		(next)->heap_prev = (prev);			\
	} else {						\
		mcb_t *prev = (curr)->free_prev;		\
		mcb_t *next = (curr)->free_next;		\
		(prev)->free_next = (next);			\
		(next)->free_prev = (prev);			\
	}							\
}

#define GET_SRC_INDEX(size,index)		\
{						\
	int hi = highbit(size);			\
	int lo = lowbit(1UL << hi);		\
	index = lo < 8 ? 0:lo-8;		\
	if (index >= N_FREELIST)		\
		index = N_FREELIST - 1;		\
}

#define GET_TGT_INDEX(size,index)		\
{						\
	GET_SRC_INDEX(size, index);		\
	if (index < N_FREELIST - 1)		\
		index--;			\
}

#ifndef MAX
#define MAX(a,b) (a>b?a:b)
#endif

struct mcb {
	struct mcb *free_prev; /* per freelist link */
	struct mcb *free_next;
	struct mcb *heap_prev; /* the link for the entire heap */
	struct mcb *heap_next;
	unsigned long size; /* size of memory.
			  start address of memory is identical to start address
		     	  of this headergment is free for alloc. so no need to
		      	  define it */
	struct {
		unsigned long flag:24;
		unsigned long magic:40;
	} bf;
#define FREELIST_AVAIL	(1) /* the memory segment is free for alloc */
#define ALLOC_MAGIC	(0x414c4c4f43) 
#define FREE_MAGIC	(0x4652454500)
#define DUMMY_MAGIC	(0x44554d4d59)	
}; 

enum list_type {
	HEAP = 1,
	FREE,
};

typedef struct mcb mcb_t;

extern void *t_malloc(size_t);
extern void t_free(void *);
extern void t_memfini(void);

#ifdef __cplusplus
}
#endif

#endif /* _MYALLOC_H */
