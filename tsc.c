// SPDX-License-Identifier: BSD-3-Clause
/*
 * tsc.c
 *
 * gcc -Wall -O2 -g -o tsc tsc.c -lpthread
 *
 * This program benchmarks the rdtscp instruction against both high and low IPC loops.
 * It allows you see how much rdtscp slows down each loop, and also compares
 * rdtscp against rdtsc.
 *
 * Example usage:
 *
 * tsc low_ipc -- the default, runs a low IPC loop with rdtscp
 * tsc low_ipc notsc -- runs a low IPC loop without rdtscp
 * tsc low_ipc rdtsc -- runs a low IPC loop with rdtsc
 * tsc low_ipc clock_gettime -- runs a low IPC loop with clock_gettime()
 * tsc low_ipc cmp -- runs a low IPC loop with and without rdtscp
 * tsc low_ipc cmp rdtsc -- runs a low IPC loop with and without rdtsc
 * tsc low_ipc cmp clock_gettime -- runs a low IPC loop with and without clock_gettime
 *
 * You can run all of the above with high_ipc instead of low_ipc
 *
 * tsc rdtscp -- just runs rdtscp to see how many calls per second it can down
 * tsc rdtsc -- just runs rdtsc to see how many calls per second it can down
 * tsc clock_gettime -- just runs clock_gettime to see how many calls per second it can down
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <locale.h>
#include <pthread.h>

#define USEC_PER_SEC 1000000

/* we want a large matrix to force cache misses */
static unsigned long matrix_size = 64 * 1024 * 1024ULL;
static unsigned long *global_matrix;
static volatile unsigned long stopping = 0;
char *tsc_variant = "rdtscp";
static volatile int skip_rdtsc = 0;
static int runtime = 10;
static int run_mode = 0;

/*
 * possible valid modes
 * low_ipc
 * low_ipc | cmp -- compares the low_ipc run with and without rdtscp reads
 * low_ipc | notsc -- just does a low_ipc run without tsc reads
 * low_ipc | [rdtsc | clock_gettime ]-- just does a low_ipc run with rdtsc instead of rdtscp
 * low_ipc | cmp | [rdtsc | clock_gettime ]-- compares the low_ipc run with and without rdtsc reads
 * (same as above but for high_ipc)
 * rdtscp
 * rdstc
 * clock_gettime
 */
enum modes {
        MODE_CMP = 1 << 0,
        MODE_LOW_IPC = 1 << 1,
        MODE_RDTSCP = 1 << 2,
        MODE_HIGH_IPC = 1 << 3,
        MODE_NO_TSC = 1 << 4,
        MODE_RDTSC = 1 << 5,
        MODE_GETTIME = 1 << 6,
};

/* use a smaller subset for high IPC tests */
static unsigned long high_ipc_matrix = 105;

typedef void *(*thread_func)(void *);

struct thread_data {
        unsigned long calls_per_sec;
};

void tvsub(struct timeval *tdiff, struct timeval *t1, struct timeval *t0)
{
	tdiff->tv_sec = t1->tv_sec - t0->tv_sec;
	tdiff->tv_usec = t1->tv_usec - t0->tv_usec;
	if (tdiff->tv_usec < 0 && tdiff->tv_sec > 0) {
		tdiff->tv_sec--;
		tdiff->tv_usec += USEC_PER_SEC;
		if (tdiff->tv_usec < 0) {
			fprintf(stderr,
				"lat_fs: tvsub shows test time ran backwards!\n");
			exit(1);
		}
	}

	/* time shouldn't go backwards */
	if (tdiff->tv_usec < 0 || t1->tv_sec < t0->tv_sec) {
		tdiff->tv_sec = 0;
		tdiff->tv_usec = 0;
	}
}

/*
 * returns the difference between start and stop in usecs.  Negative values
 * are turned into 0
 */
unsigned long long tvdelta(struct timeval *start, struct timeval *stop)
{
	struct timeval td;
	unsigned long long usecs;

	tvsub(&td, stop, start);
	usecs = td.tv_sec;
	usecs *= USEC_PER_SEC;
	usecs += td.tv_usec;
	return (usecs);
}

static inline unsigned long rdtscp(unsigned int *aux)
{
	unsigned int eax, edx;
	__asm__ __volatile__("rdtscp" : "=a"(eax), "=d"(edx), "=c"(*aux));
	return ((unsigned long)edx) << 32 | eax;
}

static inline unsigned long rdtsc(unsigned int *aux)
{
	unsigned int eax, edx;
	__asm__ __volatile__("rdtsc" : "=a"(eax), "=d"(edx), "=c"(*aux));
	return ((unsigned long)edx) << 32 | eax;
}

static __attribute__((noinline)) unsigned long read_tsc(unsigned int *aux)
{
        if (skip_rdtsc)
                return 0;
        if (run_mode & MODE_RDTSCP)
                return rdtscp(aux);
        if (run_mode & MODE_GETTIME) {
                struct timespec tsc;
                return clock_gettime(CLOCK_MONOTONIC, &tsc);
        }
        return rdtsc(aux);
}

/* just a little bit of math and a lot of cache misses */
static unsigned long low_ipc(unsigned long *loops)
{
	int i;
        int j;
        int k;
	int src = 0;
	int dst = 0;
	unsigned int aux;
        unsigned long index = 0;
        volatile unsigned long val = 0;

        if (index == 0)
                index = rand() % matrix_size;

	for (i = 0; i < 1024; i++) {
		src = global_matrix[index] % matrix_size;
                index = (index + 1) % matrix_size;
		dst = global_matrix[src] % matrix_size;

                for (j = 0; j < 256; j++) {
                        dst = global_matrix[(dst + j) % matrix_size] % matrix_size;
                        if ((i * j) % 500 == 0) {
                                val += read_tsc(&aux);
                        }
                        *loops += 1;
                }

                /*
                 * adjust this loop with more rounds in order to increase IPC
                 * the goal is around 0.5
                 */
                for (k = 0; k < 2; k++) {
                        global_matrix[dst] += global_matrix[(src + k) % matrix_size] +
                                global_matrix[(dst + k) % matrix_size];
                }
                if (stopping)
                        break;
	}
	return global_matrix[dst] + val;
}

/*
 * does our low IPC math, which bounces around in our global matrix
 * On most machines this gives us IPC of less than 1.
 */
void *low_ipc_thread(void *arg)
{
        struct thread_data *td = arg;
	unsigned long loops = 0;
	unsigned long calls_s;
	unsigned long long delta;
	struct timeval now;
	struct timeval start;

	gettimeofday(&start, NULL);
	while (!stopping) {
		low_ipc(&loops);
	}
        gettimeofday(&now, NULL);
        delta = tvdelta(&start, &now);

	calls_s = (loops * USEC_PER_SEC) / delta;
        td->calls_per_sec = calls_s;
	fprintf(stderr, "low IPC (%s%s) loops/s %'lu\n",
                skip_rdtsc ? "no " : "", tsc_variant, calls_s);
        return NULL;
}

/*
 * dumb matrix multiplication, every so often it also reads the tsc
 */
static void high_ipc(unsigned long *loops)
{
	unsigned long i, j, k;
	unsigned long *m1, *m2, *m3;
	unsigned int aux = 0;
        unsigned long ops_count = 0;

	m1 = &global_matrix[0];
	m2 = &global_matrix[high_ipc_matrix* high_ipc_matrix];
	m3 = &global_matrix[2 * high_ipc_matrix * high_ipc_matrix];

        for (i = 0; i < high_ipc_matrix; i++) {
                for (j = 0; j < high_ipc_matrix; j++) {
                        m3[i * high_ipc_matrix + j] = 0;

                        /*
                         * it doesn't matter much where we bump the loop
                         * counter, just do it every so often
                         */
                        *loops += 1;

                        for (k = 0; k < high_ipc_matrix; k++) {
                                m3[i * high_ipc_matrix + j] +=
                                        m1[i * high_ipc_matrix + k] *
                                        m2[k * high_ipc_matrix + j];
                                ops_count++;
                                if (ops_count % 500 == 0) {
                                        read_tsc(&aux);
                                }
                                if (stopping)
                                        return;
                        }
                }
        }
}

/*
 * does our high IPC matrix multiplication
 * on most machines this gives us IPC of at least 3.
 */
void *high_ipc_thread(void *arg)
{
        struct thread_data *td = arg;
	unsigned long loops = 0;
	unsigned long calls_s;
	unsigned long long delta;
	struct timeval now;
	struct timeval start;

	gettimeofday(&start, NULL);
	while (!stopping) {
		high_ipc(&loops);
	}
        gettimeofday(&now, NULL);
        delta = tvdelta(&start, &now);

	calls_s = (loops * USEC_PER_SEC) / delta;
        td->calls_per_sec = calls_s;
	fprintf(stderr, "High IPC (%s%s) loops/s %'lu\n",
                skip_rdtsc ? "no " : "", tsc_variant, calls_s);
        return NULL;
}

/*
 * reads rdtscp or rdtsc or clock_gettime in a loop
 * until stopping is set, prints out how
 * many calls per second we were able to
 */
void *read_tsc_thread(void *arg)
{
        struct thread_data *td = arg;
	unsigned long loops = 0;
	unsigned long calls_s;
	unsigned long long delta;
	struct timeval now;
	struct timeval start;
        unsigned int aux;

	gettimeofday(&start, NULL);
	while (!stopping) {
		loops++;
		read_tsc(&aux);
	}
        gettimeofday(&now, NULL);
        delta = tvdelta(&start, &now);

	calls_s = (loops * USEC_PER_SEC) / delta;
        td->calls_per_sec = calls_s;
	fprintf(stderr, "%s calls/s %'lu\n", tsc_variant, calls_s);
        return NULL;
}

/*
 * makes a thread, sleeps for N seconds, sets stopping to 1, waits for completion
 */
void run_for_secs(int secs, thread_func func, struct thread_data *td)
{
        pthread_t thread;
        int ret;

        stopping = 0;
        ret = pthread_create(&thread, NULL, func, td);
        if (ret) {
                fprintf(stderr, "pthread_create failed: %d\n", ret);
                exit(1);
        }
        sleep(secs);
        stopping = 1;
        pthread_join(thread, NULL);
}

int main(int ac, char **av)
{
	unsigned long i;
        int numbers[2048];
        struct thread_data td = { 0 };

        if (ac > 1)
                run_mode = 0;
        else
                fprintf(stderr, "running default low IPC run\n");

        for (i = 1; i < (unsigned long)ac; i++) {
                char *str = av[i];
                if (strcmp(str, "low_ipc") == 0) {
                        run_mode |= MODE_LOW_IPC;
                        fprintf(stderr, "running low IPC test\n");
                } else if (strcmp(str, "notsc") == 0) {
                        fprintf(stderr, "disabling tsc reads\n");
                        run_mode |=  MODE_NO_TSC;
                } else if (strcmp(str, "rdtscp") == 0) {
                        fprintf(stderr, "use rdtscp\n");
                        run_mode |= MODE_RDTSCP;
                } else if (strcmp(str, "rdtsc") == 0) {
                        fprintf(stderr, "use rdtsc\n");
                        tsc_variant = "rdtsc";
                        run_mode |= MODE_RDTSC;
                } else if (strcmp(str, "clock_gettime") == 0) {
                        fprintf(stderr, "use clock_gettime\n");
                        tsc_variant = "clock_gettime";
                        run_mode |= MODE_GETTIME;
                } else if (strcmp(str, "cmp") == 0) {
                        fprintf(stderr, "comparison run\n");
                        run_mode |= MODE_CMP;
                } else if (strcmp(str, "high_ipc") == 0) {
                        fprintf(stderr, "running high IPC test\n");
                        run_mode |= MODE_HIGH_IPC;
                } else {
                        fprintf(stderr, "usage: %s [low_ipc | high_ipc | rdstc[p]] [cmp] [notsc]\n", av[0]);
                        fprintf(stderr, "\ttsc low_ipc cmp runs the low_ipc math comparing rdstcp with noop\n");
                        fprintf(stderr, "\ttsc high_ipc runs the high IPC loop\n");
                        fprintf(stderr, "\ttsc rdstcp only the rdtscp instruction\n");
                        fprintf(stderr, "\tpassing notsc disables the tsc reads\n");
                        fprintf(stderr, "\tpassing rdtsc uses rdtsc instead of rdtscp\n");
                        exit(1);
                }
        }

        /* default to low_ipc if nothing was specified */
        if (!(run_mode & (MODE_LOW_IPC | MODE_HIGH_IPC | MODE_RDTSC | MODE_RDTSCP | MODE_GETTIME))) {
                run_mode |= MODE_LOW_IPC;
        }

        /* default to rdtscp if nothing was specified */
        if (!(run_mode & (MODE_RDTSC | MODE_RDTSCP | MODE_GETTIME))) {
                run_mode |= MODE_RDTSCP;
        }

	/* just so fprintf gives us %'lu formatting */
	setlocale(LC_ALL, "");

        /* the big matrix is just our way to make cache misses and lower IPC */
	global_matrix = malloc(matrix_size * sizeof(unsigned long));
	if (!global_matrix) {
		fprintf(stderr, "malloc failed\n");
		exit(1);
	}

        /* find some random numbers */
        for (i = 0; i < 2048; i++) {
                numbers[i] = rand();
        }

        /* fill the matrix with our randoms */
	for (i = 0; i < matrix_size; i++) {
		global_matrix[i] = numbers[i % 2048];
	}

        if (run_mode & MODE_LOW_IPC) {
                if (run_mode & MODE_NO_TSC)
                        skip_rdtsc = 1;

                run_for_secs(runtime, low_ipc_thread, &td);

                if (run_mode & MODE_CMP) {
                        double calls = td.calls_per_sec;
                        double skip_calls;

                        /* disable the tsc reads and run again */
                        skip_rdtsc = 1;
                        run_for_secs(runtime, low_ipc_thread, &td);
                        skip_calls = td.calls_per_sec;

                        fprintf(stderr, "ratio %.2f\n", calls / skip_calls);
                }
        } else if (run_mode & MODE_HIGH_IPC) {
                if (run_mode & MODE_NO_TSC)
                        skip_rdtsc = 1;

                run_for_secs(runtime, high_ipc_thread, &td);

                if (run_mode & MODE_CMP) {
                        double calls = td.calls_per_sec;
                        double skip_calls;

                        /* disable the tsc reads and run again */
                        skip_rdtsc = 1;
                        run_for_secs(runtime, high_ipc_thread, &td);
                        skip_calls = td.calls_per_sec;

                        fprintf(stderr, "ratio %.2f\n", calls / skip_calls);
                }
        } else if (run_mode & (MODE_RDTSCP | MODE_RDTSC | MODE_GETTIME)) {
                run_for_secs(runtime, read_tsc_thread, &td);
        }
}
