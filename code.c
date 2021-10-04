#include <unistd.h> 
#include <assert.h> 
#include <stdio.h>
#include <pthread.h>
#include "code.h"

static int highbit(unsigned long num) {
	int i;

	if (num == 0)
		return (0);
	for (i = 63; i >= 0; i--) {
		if (num & (1UL << i))
			return (i+1);
	}
	return (i+1);
}

static int lowbit(unsigned long num) {
	int i;
	if (num == 0)
		return (0);
	for (i = 0; i < 64; i++) {
		if (num & (1UL << i))
			return (i+1);
	}
	return (i+1);
}

/*
 * 12 freelists are maintained to serve alloc request in the
 * following size range.
 * 128,256,512,1k,2k,4k,8k,16k,32k,64k,128k,128k+
 * eg. all memory segments in freelist[0] have size in [128,256), so all
 * allocs with size(mcb length included) in range (0-128] can be served
 * by whichever entry in this freelist.
 * 
 * The size range for segments in last freelist is [128k,...), an alloc with
 * size larger than 128k need to check one by one in this list. Currently,
 * only the instant fit policy is supported, which means, the first segment
 * which has larger size than requested is returned.
 *
 * One more list is maintained which contains all of the memory segment, both
 * free or allocated ones, in descend order of the start address. When a segment
 * is freed, it is easy to merge with its adjacent segments if possible -- this
 * is good for fragmentation reduction 
 */
#define SBRK_CHUNK	(0x40000)
#define N_FREELIST	(12)
#define FREE_MIN	(128)
mcb_t *seglist[N_FREELIST+1];
mcb_t **freelist;
mcb_t *entire_list;
int initialized = 0;
pthread_mutex_t mymem_mutex = PTHREAD_MUTEX_INITIALIZER;

static void meminit(void) {
	int size = (N_FREELIST+1) * sizeof (mcb_t);
	int i;
	mcb_t *tmp, *flist;

	flist = (mcb_t *)sbrk(0);
	assert(sbrk(size) != (void *)(-1));
	for (i = 0, tmp = flist; i <= N_FREELIST; i++, tmp++) {
		/* act as dummy node for each of the list */
		tmp->heap_prev = tmp;
		tmp->heap_next = tmp;
		tmp->free_prev = tmp;
		tmp->free_next = tmp;
		tmp->bf.magic = DUMMY_MAGIC;
		seglist[i] = tmp;
	}
	entire_list = seglist[N_FREELIST];
	freelist = seglist;
}

static void _t_free(void *ptr) {
	mcb_t *curr_mcb, *prev_mcb, *next_mcb;
	int flist_index = 0;

	if (!ptr)
		return;

	curr_mcb = (mcb_t *)((unsigned long)ptr - sizeof(mcb_t));
	prev_mcb = curr_mcb->heap_prev;
	next_mcb = curr_mcb->heap_next;

	assert(curr_mcb->bf.magic == ALLOC_MAGIC); /* invalid ptr error */
	assert((curr_mcb->bf.flag & FREELIST_AVAIL) == 0); /* re-free error */

	/*
	 * note: the segments in the entire list are maintained in descend
	 * order of the start address.
	 * merge with its adjacents if they are also free.
	 */
	if (prev_mcb->bf.flag & FREELIST_AVAIL) {
		/*
		printf("merge prev$ prev:%d(flag:%x magic:%lx)"
			" curr:%d(flag:%x magic:%lx) curr_size:%d\n",
			prev_mcb, prev_mcb->bf.flag, prev_mcb->bf.magic, 
			curr_mcb, curr_mcb->bf.flag, curr_mcb->bf.magic, 
			curr_mcb->size);
		*/
		assert(prev_mcb->bf.magic == FREE_MAGIC);
		assert((unsigned long)prev_mcb-(unsigned long)curr_mcb ==
		     	curr_mcb->size);
		LIST_REMOVE(prev_mcb, HEAP);
		if (prev_mcb->size >= FREE_MIN) {
		       	/* only segmentlarger than FREE_MIN are in freelist*/
			LIST_REMOVE(prev_mcb, FREE);
		}
		curr_mcb->size += prev_mcb->size;
	}
	if (next_mcb->bf.flag & FREELIST_AVAIL) {
		/*
		printf("merge next$ curr:%d(flag:%x magic:%lx)"
			" next:%d(flag:%x magic:%lx) next_size:%d\n",
			curr_mcb, curr_mcb->bf.flag, curr_mcb->bf.magic, 
			next_mcb, next_mcb->bf.flag, next_mcb->bf.magic, 
			next_mcb->size);
		*/
		assert(next_mcb->bf.magic == FREE_MAGIC);
		assert((unsigned long)curr_mcb-(unsigned long)next_mcb ==
			next_mcb->size);
		LIST_REMOVE(curr_mcb, HEAP);
		if (next_mcb->size >= FREE_MIN) {
		       	/* only segmentlarger than FREE_MIN are in freelist*/
			LIST_REMOVE(next_mcb, FREE);
		}
		next_mcb->size += curr_mcb->size;
		curr_mcb = next_mcb;
	}
	GET_TGT_INDEX(curr_mcb->size, flist_index);
	curr_mcb->bf.magic = FREE_MAGIC;
	curr_mcb->bf.flag |= FREELIST_AVAIL;
	if (flist_index >= 0) { 
		LIST_INSERT(freelist[flist_index], curr_mcb, FREE);
	}
	return; 
} 

static void *_t_malloc(size_t sz) { 
	int flist_index = 0;
	size_t numbytes;
	unsigned long oldsbrk, growsize;
	mcb_t *curr_mcb = NULL, *new_mcb;

	if (!initialized) {
		meminit();
		initialized = 1;
	}
	if (sz <= 0)
		return NULL;
	numbytes = sz + sizeof(struct mcb); 
	GET_SRC_INDEX(numbytes, flist_index);
	/*
	 * For alloc request falling in size range of all freelists other
	 * than the last one, if its corresponding freelist is not empty,
	 * since all entries in the freelist are qualified target, we just
	 * get the first one.
	 *
	 * If the request falls into the last freelist, or the last freelist
	 * is not empty, try to get one in this freelist first.
	 * If it still doesn't get one there, then grow the
	 * heap. The size to grow is multiple times of the SBRK_CHUNK. 
	 * The remaining parts of the grown is put on the corresonding target
	 * freelist.
	 *
	 * All memory segments with size in range (0,128) are not put on any
	 * freelist, instead, there are just put on the entire list. 
	 *
	 * When segments are freed, try to merge with the adjacents to reduce
	 * fragmentation.
	 */
	if ((flist_index != N_FREELIST-1) &&
		(freelist[flist_index]->free_next) != freelist[flist_index]) {
		curr_mcb = freelist[flist_index]->free_next;
	} else {
		/*
		 * when allocs fall in this catergory, we need check one by one
		 * till we get one that meets requirement.
		 */
		mcb_t *loop = freelist[N_FREELIST-1]->free_next;
		while (loop != freelist[N_FREELIST-1]) {
			/*printf("%d:%d\n", loop->size, numbytes);*/
			if (loop->size < numbytes) {
				loop = loop->free_next;
				continue;
			}
			curr_mcb = loop;
			break;
		}
		if (!curr_mcb) { /* still not get it? then grow */
			growsize = (numbytes + (SBRK_CHUNK-1))
			    / SBRK_CHUNK * SBRK_CHUNK;
			oldsbrk = (unsigned long)sbrk(0);
			if (sbrk(growsize) == (void *)(-1))
				return NULL;
			curr_mcb = (mcb_t *)oldsbrk;
			curr_mcb->size = (growsize - numbytes > sizeof(mcb_t) ?
				numbytes:growsize);
			curr_mcb->bf.magic = ALLOC_MAGIC;
			/* put this segment in the entire list */
			LIST_INSERT(entire_list, curr_mcb, HEAP);
			assert(curr_mcb->heap_next == entire_list ||
				curr_mcb->heap_next < curr_mcb);	
			assert(curr_mcb->heap_prev == entire_list ||
				curr_mcb->heap_prev > curr_mcb);	
			if (growsize - numbytes > sizeof(mcb_t)) {
				new_mcb = (mcb_t *)(oldsbrk + numbytes);
				new_mcb->size = growsize - numbytes;
				new_mcb->bf.magic = FREE_MAGIC;
				new_mcb->bf.flag |= FREELIST_AVAIL;
				/*
				 * the remaining is linked to the entire list
				 * too. if the remaining is larger than 128,
				 * the segment is also linked to the corresponding
				 * freelist.
				 * note: flist_index == -1 means the size is
				 * in (0,128) range.
				 */
				LIST_INSERT(curr_mcb->heap_prev, new_mcb, HEAP);
				assert(new_mcb->heap_next == entire_list ||
					new_mcb->heap_next < new_mcb);	
				assert(new_mcb->heap_prev == entire_list ||
					new_mcb->heap_prev > new_mcb);	
				GET_TGT_INDEX(growsize - numbytes, flist_index);
				if (flist_index >= 0) { 
					LIST_INSERT(freelist[flist_index],
						new_mcb, FREE);
				}
			}
			return ((void *)(oldsbrk + sizeof(mcb_t)));
		}
	}

	/*
	 * The freelist is not empty. Get the first from that list, if
	 * 1. the segment has exact same size, remove it from the freelist.
	 * note: the segment is on the entire heap list already, just set the
	 * 'allocated' flag
	 * 2. the segment has larger size, then link the remaining to the
	 * corresponding freelist if necessary, and also create and insert
	 * a new segment in the entire heap list
	 */
	/*printf("alloc %d:%d\n", curr_mcb->size, numbytes);*/
	assert(curr_mcb != NULL);
	assert(curr_mcb->size >= numbytes);
	/* 
	 * remove this segment from the freelist first, insert to its 
	 * corresponding freelist if necessary later
	 */
	LIST_REMOVE(curr_mcb, FREE);
	if (curr_mcb->size <= numbytes + sizeof(mcb_t)) {
		/* 
		 * perfect fit, or the remaining is even smaller than a mcb,
		 * then just mark the segment as 'allocated' in entire list
		 */
		curr_mcb->bf.magic = ALLOC_MAGIC;
		curr_mcb->bf.flag &= (~FREELIST_AVAIL);
	} else {
		new_mcb = (mcb_t *)((unsigned long)curr_mcb + numbytes);
		new_mcb->size = curr_mcb->size - numbytes;
		new_mcb->bf.magic = FREE_MAGIC;
		new_mcb->bf.flag |= FREELIST_AVAIL;
		curr_mcb->size = numbytes;
		curr_mcb->bf.magic = ALLOC_MAGIC;
		curr_mcb->bf.flag &= (~FREELIST_AVAIL);
		LIST_INSERT(curr_mcb->heap_prev, new_mcb, HEAP);
		assert(new_mcb->heap_next == entire_list ||
			new_mcb->heap_next < new_mcb);	
		assert(new_mcb->heap_prev == entire_list ||
			new_mcb->heap_prev > new_mcb);	
		GET_TGT_INDEX(new_mcb->size, flist_index);
		if (flist_index >= 0) { 
			LIST_INSERT(freelist[flist_index], new_mcb, FREE);
		}
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
 * TODO: all some statistics here
 */
void t_memfini(void) {
	int i;
	for (i = 0; i < N_FREELIST-1; i++) {
		assert(freelist[i]->free_next == freelist[i]);
		assert(freelist[i]->free_prev == freelist[i]);
	}
	assert(freelist[i]->free_next->free_next == freelist[i]);
	assert(freelist[i]->free_prev->free_prev == freelist[i]);
	assert(freelist[i]->free_next->bf.magic == FREE_MAGIC);
	assert(entire_list->heap_next->heap_next == entire_list);
	assert(entire_list->heap_prev->heap_prev == entire_list);
	assert(entire_list->heap_next->bf.magic == FREE_MAGIC);
}
