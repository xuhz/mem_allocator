/******************************************************************************
* Copyright (C) 2016-2022 Brian Xu  All rights reserved.
* SPDX-License-Identifier: MIT
* author: xuhuazhuo@gmail.com
******************************************************************************/
#ifndef _MYALLOC_H
#define _MYALLOC_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIST_EMPTY(list)			\
({						\
	(list)->next == (list);			\
})

#define	LIST_INSERT_AFTER(pos, node)		\
{						\
	LIST *next = (pos)->next;		\
	(node)->next = (next);			\
	(node)->prev = (pos);			\
	(pos)->next = (node);			\
	(next)->prev = (node);			\
}

#define	LIST_REMOVE(node)			\
{						\
	LIST *prev = (node)->prev;		\
	LIST *next = (node)->next;		\
	(prev)->next = (next);			\
	(next)->prev = (prev);			\
	(node)->next = NULL;			\
	(node)->prev = NULL;			\
}

#define NODE_OWNER(node, member) ({ 			\
	void *ptr = (void *)node; 			\
	((mcb_t *)(ptr - offsetof(mcb_t, member)));	\
	})

#ifndef MAX
#define MAX(a,b) (a>b?a:b)
#endif

typedef struct list {
	struct list *prev;
	struct list *next;
} LIST;

typedef struct mcb {
	LIST free_node; /* per freelist link */
	LIST heap_node; /* the link for the entire heap */
	size_t size; /* size of memory item */
	union {
		struct {
			unsigned int flag:16;
			unsigned int magic:16;
		};
		unsigned long nouse;
	};
#define FREELIST_AVAIL	(1U) /* the memory segment is free for alloc */
#define ALLOC_MAGIC	(0x1357) 
#define FREE_MAGIC	(0x2468)
} mcb_t; 

extern void *t_malloc(size_t);
extern void t_free(void *);
extern void t_memfini(void);

#ifdef __cplusplus
}
#endif

#endif /* _MYALLOC_H */
