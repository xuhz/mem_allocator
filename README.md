* An implementation of a memory allocator

```
In this implementation, 12 corrals are defined to serve alloc request in the
following size range.
(0-128], (128-256], (256-512], ... , (64k-128k], (128k+)

12 freelists are maintained so that all items in one list can meet the requirement
from the corresponding corral.  
eg. all memory segments in freelist[0] have size in [128,256), so all
allocs with size(mcb length included) in range (0-128] can be served
by whichever entry in this freelist.

malloc_size	correl_index	object_size_in_freelist	freelist_index
(0-128]		0				[128-256)				0
(128-256]	1				[256-512)				1
...
(64k-128k]	10				[128k]					10  
(128k+]		11				[128k+] 				11

The size range for segments in last freelist is [128k,...), an alloc with
size larger than 128k need to check one by one in this list. Currently,
only the instant fit policy is supported, which means, the first segment
which has larger size than requested is returned.

After initialization, all freelist are empty. The 1st alloc will cause the
last freelist expand (by multiple of 256k). the requested size is returned,
the remaining is put into the corresponding freelist for future alloc request.
The expand will happen any time no freelist can fulfil the alloc request.

During free, the item will return to its corresponding freelist. 

One more list is maintained which contains all of the memory segment, both
free or allocated ones, in descend order of the start address. When a segment
is freed, it is easy to merge with its adjacent segments if possible -- this
is good for fragmentation reduction 

```

## build

$> make

## test

```
$ ./mytest 
How many seconds do you want to run?
20
check freelist: 0
	 alloc: 62870
	 free: 33349
	 alloc_from_last_corral: 4500
	 defrag_during_free: 157066
check freelist: 1
	 alloc: 121510
	 free: 61964
	 alloc_from_last_corral: 925
	 defrag_during_free: 267923
check freelist: 2
	 alloc: 243150
	 free: 142097
	 alloc_from_last_corral: 335
	 defrag_during_free: 514555
check freelist: 3
	 alloc: 484294
	 free: 334264
	 alloc_from_last_corral: 3571
	 defrag_during_free: 1004767
check freelist: 4
	 alloc: 872677
	 free: 869224
	 alloc_from_last_corral: 103808
	 defrag_during_free: 1795878
check freelist: 5
	 alloc: 814368
	 free: 507808
	 alloc_from_last_corral: 1138622
	 defrag_during_free: 276305
check freelist: 6
	 alloc: 293541
	 free: 330063
	 alloc_from_last_corral: 130
	 defrag_during_free: 495401
check freelist: 7
	 alloc: 483806
	 free: 595551
	 alloc_from_last_corral: 2062
	 defrag_during_free: 1011742
check freelist: 8
	 alloc: 872819
	 free: 1099675
	 alloc_from_last_corral: 99072
	 defrag_during_free: 1843401
check freelist: 9
	 alloc: 661863
	 free: 540865
	 alloc_from_last_corral: 1284067
	 defrag_during_free: 236618
check freelist: 10
	 alloc: 4
	 free: 3
	 alloc_from_last_corral: 231937
	 defrag_during_free: 0
check freelist: 11
	 alloc: 0
	 free: 6470177
	 alloc_from_last_corral: 3205109
	 defrag_during_free: 3249454
	 One item in last freelist. That is expected!
check heaplist:
	 One item in heaplist. That is expected!
malloc: 10985040 times  free: 10985040 times

Succeed!!!

```


