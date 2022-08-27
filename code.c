/******************************************************************************
* Copyright (C) 2016-2022 Brian Xu  All rights reserved.
* SPDX-License-Identifier: MIT
* author: xuhuazhuo@gmail.com
******************************************************************************/

#include <unistd.h> 
#include <assert.h> 
#include <stdio.h>
#include <stddef.h>
#include <pthread.h>
#include "code.h"

//#define DEBUG
#define STATISTICS
#ifdef DEBUG
#define DBG printf
#else
#define DBG
#endif

/*
 * 12 corrals are maintained to serve alloc request in the
 * following size range.
 * 128,256,512,1k,2k,4k,8k,16k,32k,64k,128k,128k+
 * eg. all memory segments in freelist[0] have size in [128,256), so all
 * allocs with size(mcb length included) in range (0-128] can be served
 * by whichever entry in this freelist.
 * malloc_size	correl_index	object_size_in_freelist	freelist_index
 * (0-128]		0				[128-256)				0
 * (128-256]	1				[256-512)				1
 * ...
 * (64k-128k]	10				[128k-256k)				10  
 * (128k+]		11				[128k+] 				11
 * 
 * The size range for segments in last freelist is [128k,...), an alloc with
 * size larger than 128k need to check one by one in this list. Currently,
 * only the instant fit policy is supported, which means, the first segment
 * which has larger size than requested is returned.
 *
 * After initialization, all freelist are empty. The 1st alloc will cause the
 * last freelist expand (by multiple of 256k). the requested size is returned,
 * the remaining is put into the corresponding freelist for future alloc request.
 * The expand will happen any time no freelist can fulfil the alloc request.
 *
 * During free, the item will return to its corresponding freelist. 
 *
 * One more list is maintained which contains all of the memory segment, both
 * free or allocated ones, in descend order of the start address. When a segment
 * is freed, it is easy to merge with its adjacent segments if possible -- this
 * is good for fragmentation reduction 
 */
#define SBRK_CHUNK	(0x40000)

size_t search_table[] = {
	1U << 7, /*128*/
	1U << 8, /*256*/
	1U << 9, /*512*/
	1U << 10, /*1k*/
	1U << 11, /*2k*/
	1U << 12, /*4k*/
	1U << 13, /*8k*/
	1U << 14, /*16k*/
	1U << 15, /*32k*/
	1U << 16, /*64k*/
	1U << 17, /*128k*/
	1U << 18, /*128k+*/
};

#define N_FREELIST	(sizeof (search_table) / sizeof (size_t))
#define FREE_MIN	(search_table[0])

LIST *seglist[N_FREELIST+1];
LIST **freelist;
LIST *entire_list;
#ifdef STATISTICS
typedef struct stat {
	char* object_sz;
	size_t alloc;
	size_t free;
	size_t alloc_from_last_corral;
	size_t defrag_during_free;
} stat_t;
stat_t showstat[N_FREELIST] = {
	{
		.object_sz = "[128-256)",
	},
	{
		.object_sz = "[256-512)",
	},
	{
		.object_sz = "[512-1k)",
	},
	{
		.object_sz = "[1k-2k)",
	},
	{
		.object_sz = "[2k-4k)",
	},
	{
		.object_sz = "[4k-8k)",
	},
	{
		.object_sz = "[8k-16k)",
	},
	{
		.object_sz = "[16k-32k)",
	},
	{
		.object_sz = "[32k-64k)",
	},
	{
		.object_sz = "[64k-128k)",
	},
	{
		.object_sz = "[128k+)",
	},
};
size_t malloc_sum = 0, free_sum = 0;
#endif

int initialized = 0;
pthread_mutex_t mymem_mutex = PTHREAD_MUTEX_INITIALIZER;

static LIST *get_node_from_last_list(size_t numbytes) {
	unsigned long oldsbrk, growsize;
	mcb_t *new_mcb;
	int found = 0;
	LIST *dummy = freelist[N_FREELIST-1], *node = dummy->next;

	/*
	 * when allocs fall in the last list, we need check one by one
	 * till we get one that meets requirement.
	 */
	if (!LIST_EMPTY(dummy)) {
		while (node != dummy) {
			if (NODE_OWNER(node, free_node)->size < numbytes) {
				node = node->next;
				continue;
			}
			found = 1;
			DBG("last list> object size %ld : request size %ld\n",
				NODE_OWNER(node, free_node)->size, numbytes);
			LIST_REMOVE(node);
			break;
		}
	}
	if (!found) {
		/* still not get it? then grow */
		growsize = (numbytes + (SBRK_CHUNK-1)) / SBRK_CHUNK * SBRK_CHUNK; 
		oldsbrk = (unsigned long)sbrk(0);
		if (sbrk(growsize) == (void *)(-1))
			return NULL;

		DBG("last list> create new object size %ld : request size %ld\n",
			growsize, numbytes);
		new_mcb = (mcb_t *)oldsbrk;
		new_mcb->size = growsize;
		new_mcb->magic = FREE_MAGIC;
		new_mcb->free_node.prev = NULL;
		new_mcb->free_node.next = NULL;

		/* 
		 * put this segment in the entire list
		 * don't put this in the freelist since it is already
		 * the candidate that will return to caller.
		 */
		LIST_INSERT_AFTER(entire_list, &new_mcb->heap_node);

		node = &new_mcb->free_node;
	}

	return node;
}

static int get_corral_index(size_t size) {
	int l, h, m;

	l = 0;
	h = N_FREELIST-1;
	while (l < h - 1) {
		m = l + (h - l) / 2;
		if (size == search_table[m])
			return m;
		else if (size < search_table[m])
			h = m;
		else
			l = m;	
	}
	if (size <= search_table[l])
		return l;
	return h;
}

static int get_dst_index(size_t size) {
	int ret = get_corral_index(size);
	if (search_table[ret] == size || ret == N_FREELIST-1)
		return ret;
	return ret-1;
}

static void meminit(void) {
	int size = (N_FREELIST+1) * sizeof (LIST);
	int i;
	LIST *tmp, *flist;

	flist = (LIST *)sbrk(0);
	assert(sbrk(size) != (void *)(-1));
	for (i = 0, tmp = flist; i <= N_FREELIST; i++, tmp++) {
		/* act as dummy node for each of the list */
		tmp->prev = tmp;
		tmp->next = tmp;
		seglist[i] = tmp;
	}
	entire_list = seglist[N_FREELIST];
	freelist = seglist;
}

static void _t_free(void *ptr) {
	mcb_t *curr_mcb, *prev_mcb, *next_mcb;
	LIST *curr_node, *prev_node, *next_node;
	int flist_index;

#ifdef STATISTICS
	free_sum++;
#endif
	if (!ptr)
		return;

	curr_mcb = (mcb_t *)((unsigned long)ptr - sizeof(mcb_t));
	curr_node = &curr_mcb->heap_node;
	prev_node = curr_node->prev;
	next_node = curr_node->next;
	prev_mcb = NODE_OWNER(prev_node, heap_node);
	next_mcb = NODE_OWNER(next_node, heap_node);

#ifdef STATISTICS
	flist_index = get_dst_index(curr_mcb->size);
#endif
	assert(curr_mcb->magic == ALLOC_MAGIC); /* invalid ptr error */

	/*
	 * When segments are freed, try to merge with the adjacents to reduce
	 * fragmentation.
	 * note: the segments in the entire list are maintained in descend
	 * order of the start address.
	 * merge with its adjacents if they are also free.
	 */
	if (prev_mcb->magic == FREE_MAGIC) {
		DBG("free> merge prev$ prev:%pmagic:%x)"
			" curr:%p(magic:%x) curr_size:%ld\n",
			prev_mcb, prev_mcb->magic, 
			curr_mcb, curr_mcb->magic, 
			curr_mcb->size);

		assert(prev_mcb->magic == FREE_MAGIC);
		assert((unsigned long)prev_mcb - (unsigned long)curr_mcb ==
			curr_mcb->size);
		LIST_REMOVE(prev_node); /* remove heap node of prev */
		assert(prev_mcb->size >= FREE_MIN);
		LIST_REMOVE(&prev_mcb->free_node); /* remove free node of prev */

		curr_mcb->size += prev_mcb->size;
#ifdef STATISTICS
	showstat[flist_index].defrag_during_free++;
#endif
	}
	if (next_mcb->magic == FREE_MAGIC) {
		DBG("free> merge next$ curr:%p(magic:%x)"
			" next:%p(magic:%x) next_size:%ld\n",
			curr_mcb, curr_mcb->magic, 
			next_mcb, next_mcb->magic, 
			next_mcb->size);

		assert(next_mcb->magic == FREE_MAGIC);
		assert((unsigned long)curr_mcb-(unsigned long)next_mcb ==
			next_mcb->size);
		LIST_REMOVE(curr_node); /* remove heap node of cur */
		assert(next_mcb->size >= FREE_MIN);
		LIST_REMOVE(&next_mcb->free_node); /* remove free node of next */

		next_mcb->size += curr_mcb->size;
		curr_mcb = next_mcb;
#ifdef STATISTICS
		showstat[flist_index].defrag_during_free++;
#endif
	}

	/* insert the merged or itself to its corresponding freelist */
	flist_index = get_dst_index(curr_mcb->size);
	assert(flist_index >= 0);
	curr_mcb->magic = FREE_MAGIC;
	LIST_INSERT_AFTER(freelist[flist_index], &curr_mcb->free_node);
#ifdef STATISTICS
	showstat[flist_index].free++;
#endif

	DBG("free> object candidate %ld inserted into list %d\n",
		curr_mcb->size, flist_index);
} 

static void *_t_malloc(size_t sz) { 
	int flist_index, corral_index;
	size_t numbytes;
	LIST *node;
	mcb_t *curr_mcb = NULL, *new_mcb;

	if (!initialized) {
		meminit();
		initialized = 1;
	}
#ifdef STATISTICS
	malloc_sum++;
#endif
	if (sz <= 0)
		return NULL;
	numbytes = sz + sizeof(struct mcb); 
	numbytes = MAX(numbytes, FREE_MIN);
	corral_index = get_corral_index(numbytes);
	/*
	 * For alloc request in size range of all freelists other
	 * than the last one, if its corresponding freelist is not empty,
	 * since all entries in the freelist are qualified target, we just
	 * get the first one.
	 *
	 * If the request falls into the last freelist, try to get one in this
	 * list by checking one by one.
	 * If it doesn't get one there, then grow the heap. The size to grow
	 * is multiple times of the SBRK_CHUNK. The grown memory will be
	 * considerred as one item and linked to the last list 
	 *
	 */
	if ((corral_index != N_FREELIST-1) && !LIST_EMPTY(freelist[corral_index])) {
		node = freelist[corral_index]->next;
		LIST_REMOVE(node);
		DBG("malloc> object candidate from list %d for request: %ld \n",
			corral_index, numbytes);
#ifdef STATISTICS
		showstat[corral_index].alloc++;
#endif
	} else {
		node = get_node_from_last_list(numbytes);
		if (!node)
			return NULL;
		DBG("malloc> object candidate from last list %ld for request: %ld \n",
			N_FREELIST-1, numbytes);
#ifdef STATISTICS
		showstat[corral_index].alloc_from_last_corral++;
#endif
	}

	curr_mcb = NODE_OWNER(node, free_node);
	/* 
	 * After the requested size is taken from the segment, if
	 * 1. the segment doesn't have big enough space to fit the smallest
	 * request, then need to create a new object
	 * note: the segment is on the entire heap list already, just set the
	 * 'allocated' flag
	 * 2. the segment has larger size, then link the remaining to the
	 * corresponding freelist if necessary, and also create and insert
	 * a new segment in the entire heap list
	 */
	/*printf("alloc %d:%d\n", curr_mcb->size, numbytes);*/
	assert(curr_mcb != NULL);
	assert(curr_mcb->size >= numbytes);
	if (curr_mcb->size < numbytes + FREE_MIN) {
		/* 
		 * perfect fit, or the remaining is even smaller than a
		 * smallest request, then just mark the segment as 'allocated'
		 * in entire list
		 */
		curr_mcb->magic = ALLOC_MAGIC;
		DBG("malloc> %ld from %ld doesn't create new object\n",
			numbytes, curr_mcb->size);
	} else {
		/* 
	 	 * node has been removed from the freelist.
		 * there will be a new memory object created. insert it to the
		 * heap list and then insert to its 
	 	 * corresponding freelist if necessary later
	 	 */
		new_mcb = (mcb_t *)((unsigned long)curr_mcb + numbytes);
		new_mcb->size = curr_mcb->size - numbytes;
		new_mcb->magic = FREE_MAGIC;
		
		/* insert the new one to heap list */
		LIST_INSERT_AFTER((&curr_mcb->heap_node)->prev, &new_mcb->heap_node);

		/* insert the remaining to its corresponding freelist */
		flist_index = get_dst_index(new_mcb->size);
		assert(flist_index >= 0);
		LIST_INSERT_AFTER(freelist[flist_index], &new_mcb->free_node);
		DBG("malloc> %ld from %ld create new object %ld in freelist: %d\n",
			numbytes, curr_mcb->size, new_mcb->size, flist_index);

		/* adject the curr_mcb size. curr_mcb is already in heap list */
		curr_mcb->size = numbytes;
		curr_mcb->magic = ALLOC_MAGIC;
	}
	return ((void*)((unsigned long)curr_mcb + sizeof(mcb_t))); 
}

void t_free(void *ptr) {
	(void) pthread_mutex_lock(&mymem_mutex);
	_t_free(ptr);
	(void) pthread_mutex_unlock(&mymem_mutex);
}

void *t_malloc(size_t sz) {
	void *ret;
	(void) pthread_mutex_lock(&mymem_mutex);
	ret = _t_malloc(sz);
	(void) pthread_mutex_unlock(&mymem_mutex);
	return (ret);
}

/*
 * This function is for test/debug purpose only.
 * If everything works fine, and there is no memory leak, then
 * the last freelist should contain only one big segment. The same
 * to the entire_list.
 * TODO: add some statistics here
 */
void t_memfini(void) {
	int i;
	for (i = 0; i < N_FREELIST - 1; i++) {
#ifdef STATISTICS
		printf("check freelist: %d\n", i);
		printf("\t alloc: %ld\n", showstat[i].alloc);
		printf("\t free: %ld\n", showstat[i].free);
		printf("\t alloc_from_last_corral: %ld\n", showstat[i].alloc_from_last_corral);
		printf("\t defrag_during_free: %ld\n", showstat[i].defrag_during_free);
#endif
		assert(freelist[i]->next == freelist[i]);
		assert(freelist[i]->prev == freelist[i]);
	}
	assert(NODE_OWNER(freelist[i]->next, free_node)->magic == FREE_MAGIC);
	assert(entire_list->next->next == entire_list);
	assert(entire_list->prev->prev == entire_list);
	assert(NODE_OWNER(entire_list->next, heap_node)->magic == FREE_MAGIC);
#ifdef STATISTICS
		printf("check freelist: %d\n", i);
		printf("\t alloc: %ld\n", showstat[i].alloc);
		printf("\t free: %ld\n", showstat[i].free);
		printf("\t alloc_from_last_corral: %ld\n", showstat[i].alloc_from_last_corral);
		printf("\t defrag_during_free: %ld\n", showstat[i].defrag_during_free);
		printf("\t One item in last freelist. That is expected!\n"); 
		printf("check heaplist:\n"); 
		printf("\t One item in heaplist. That is expected!\n"); 
		printf("malloc: %ld times  free: %ld times\n", malloc_sum, free_sum);
#endif
		printf("\nSucceed!!!\n"); 
}
