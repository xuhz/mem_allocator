#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>

extern void t_free(void*);
extern void *t_malloc(size_t);
extern void t_memfini(void);
static int done;

static void *workthread(void *arg) {
#define LOOP 10
	int i;
	void *addr[LOOP];
	struct timespec tim;
	
	tim.tv_sec = 0;
	tim.tv_nsec = lrand48() % 2000000;

	while (!done) {
		for (i = 0; i < LOOP; i++)
			addr[i] = t_malloc(10 + lrand48() % 0x100000);
		nanosleep(&tim, NULL);
		for (i = 0; i < LOOP; i++)
			t_free(addr[i]);
		nanosleep(&tim, NULL);
		for (i = 0; i < LOOP; i++)
			addr[i] = t_malloc(10 + lrand48() % 0x10000);
		nanosleep(&tim, NULL);
		for (i = 0; i < LOOP; i++)
			t_free(addr[i]);
		nanosleep(&tim, NULL);
		for (i = 0; i < LOOP; i++)
			addr[i] = t_malloc(10 + lrand48() % 0x1000);
		nanosleep(&tim, NULL);
		for (i = 0; i < LOOP; i++)
			t_free(addr[i]);
	}
}

#define NUM 50
int main(int argc, char **argv) {
	pthread_t tid[NUM];
	int i, sec;

	printf("How many seconds do you want to run?\n");
	scanf("%d", &sec);
	done = 0;
	for (i = 0; i < NUM; i++) {
		pthread_create(&tid[i], 0, workthread, 0);
	}
	sleep(sec);
	done = 1;

	for (i = 0; i < NUM; i++) {
		pthread_join(tid[i], NULL);
	}
	t_memfini();

	return (0);
}
