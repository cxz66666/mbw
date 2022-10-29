/*
 * vim: ai ts=4 sts=4 sw=4 cinoptions=>4 expandtab
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <chrono>
#include <sys/shm.h>
#include <numaif.h>
#include <vector>
#include "FastMemcpy_Avx.h"
// #include "fast_avx.h"
/* how many runs to average by default */
#define DEFAULT_NR_LOOPS 10

/* we have 3 tests at the moment */
#define MAX_TESTS 3

/* default block size for test 2, in bytes */
#define DEFAULT_BLOCK_SIZE 262144

/* test types */
#define TEST_MEMCPY 0
#define TEST_DUMB 1
#define TEST_MCBLOCK 2

/* version number */
#define VERSION "1.5"
int thread_num = 1;
volatile int begin = 0;
std::vector<char *> src_ptr;
std::vector<char *> dst_ptr;
std::vector<double> spend_time;
/*
 * MBW memory bandwidth benchmark
 *
 * 2006, 2012 Andras.Horvath@gmail.com
 * 2013 j.m.slocum@gmail.com
 * (Special thanks to Stephen Pasich)
 *
 * http://github.com/raas/mbw
 *
 * compile with:
 *			gcc -O -o mbw mbw.c
 *
 * run with eg.:
 *
 *			./mbw 300
 *
 * or './mbw -h' for help
 *
 * watch out for swap usage (or turn off swap)
 */

void usage()
{
    printf("mbw memory benchmark v%s, https://github.com/raas/mbw\n", VERSION);
    printf("Usage: mbw [options] array_size_in_MiB\n");
    printf("Options:\n");
    printf("	-n: number of runs per test (0 to run forever)\n");
    printf("	-a: Don't display average\n");
    printf("	-t%d: memcpy test\n", TEST_MEMCPY);
    printf("	-t%d: dumb (b[i]=a[i] style) test\n", TEST_DUMB);
    printf("	-t%d: memcpy test with fixed block size\n", TEST_MCBLOCK);
    printf("	-b <size>: block size in bytes for -t2 (default: %d)\n", DEFAULT_BLOCK_SIZE);
    printf("	-q: quiet (print statistics only)\n");
    printf("(will then use two arrays, watch out for swapping)\n");
    printf("'Bandwidth' is amount of data copied over the time this operation took.\n");
    printf("\nThe default is to run all tests available.\n");
}

/* ------------------------------------------------------ */

/* allocate a test array and fill it with data
 * so as to force Linux to _really_ allocate it */
long *make_array(unsigned long long asize)
{
    unsigned long long t;
    unsigned int long_size = sizeof(long);
    long *a;

    a = static_cast<long *>(calloc(asize, long_size));

    if (NULL == a)
    {
        perror("Error allocating memory");
        exit(1);
    }

    /* make sure both arrays are allocated, fill with pattern */
    for (t = 0; t < asize; t++)
    {
        a[t] = 0xaa;
    }
    return a;
}
template <uint64_t power_of_two_number, typename T>
static constexpr inline T round_up(T x)
{
    return ((x) + T(power_of_two_number - 1)) & (~T(power_of_two_number - 1));
}
char *get_huge_mem(int numa_node, size_t size)
{
    size = round_up<2 * 1024 * 1024>(size);
    int shm_key, shm_id;
    while (true)
    {
        // Choose a positive SHM key. Negative is fine but it looks scary in the
        // error message.
        shm_key = rand();
        shm_key = std::abs(shm_key);

        // Try to get an SHM region
        shm_id = shmget(shm_key, size, IPC_CREAT | IPC_EXCL | 0666 | SHM_HUGETLB);

        if (shm_id == -1)
        {
            switch (errno)
            {
            case EEXIST:
                continue; // shm_key already exists. Try again.

            case EACCES:
                exit(-1);

            case EINVAL:
                exit(-1);

            case ENOMEM:
                // Out of memory - this is OK
                printf("OOM killed\n");
                exit(-1);

            default:
                printf("unexpect error \n");
                exit(-1);
            }
        }
        else
        {
            // shm_key worked. Break out of the while loop.
            break;
        }
    }
    char *shm_buf = static_cast<char *>(shmat(shm_id, nullptr, 0));
    shmctl(shm_id, IPC_RMID, nullptr);
    const unsigned long nodemask =
        (1ul << static_cast<unsigned long>(numa_node));
    long ret = mbind(shm_buf, size, MPOL_BIND, &nodemask, 32, 0);
    if (ret)
    {
        printf("mbind error %ld", ret);
        exit(-1);
    }
    return shm_buf;
}
void bind_to_core(std::thread &thread,
                  size_t index)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    const size_t global_index = index;

    CPU_SET(global_index, &cpuset);
    int rc = pthread_setaffinity_np(thread.native_handle(), sizeof(cpu_set_t),
                                    &cpuset);
    if (rc)
    {
        exit(-1);
    }
}
void thread_func(int index, unsigned long long block_size, unsigned long long total)
{
    // wait to bind core
    std::this_thread::sleep_for(std::chrono::seconds(1));

    char *src = src_ptr[index];
    char *dst = dst_ptr[index];
    unsigned long long t = total;
    struct timeval starttime, endtime;

    while (!begin)
    {
    }
    gettimeofday(&starttime, NULL);

    for (; t >= block_size; t -= block_size, src += block_size)
    {
        // printf("[thread %d] %ld %ld\n", index, reinterpret_cast<size_t>(src) % 32, reinterpret_cast<size_t>(dst) % 32);
        dst = (char *)memcpy_fast(dst, src, block_size) + block_size;
    }
    if (t)
    {
        dst = (char *)memcpy_fast(dst, src, t) + t;
    }
    gettimeofday(&endtime, NULL);
    spend_time[index] = ((double)(endtime.tv_sec * 1000000 - starttime.tv_sec * 1000000 + endtime.tv_usec - starttime.tv_usec)) / 1000000;
}
/* actual benchmark */
/* asize: number of type 'long' elements in test arrays
 * long_size: sizeof(long) cached
 * type: 0=use memcpy, 1=use dumb copy loop (whatever GCC thinks best)
 *
 * return value: elapsed time in seconds
 */
double worker(unsigned long long asize, int type, unsigned long long block_size)
{
    unsigned long long t;
    struct timeval starttime, endtime;
    double te;
    unsigned int long_size = sizeof(long);
    /* array size in bytes */
    unsigned long long array_bytes = asize * long_size;

    std::vector<std::thread> threads(thread_num);
    for (int i = 0; i < thread_num; i++)
    {
        threads[i] = std::thread(thread_func, i, block_size, array_bytes);
        bind_to_core(threads[i], i);
    }
    std::this_thread::sleep_for(std::chrono::seconds(5));

    gettimeofday(&starttime, NULL);
    begin = 1;
    for (int i = 0; i < thread_num; i++)
    {
        threads[i].join();
    }
    gettimeofday(&endtime, NULL);

    te = ((double)(endtime.tv_sec * 1000000 - starttime.tv_sec * 1000000 + endtime.tv_usec - starttime.tv_usec)) / 1000000;
    begin = 0;
    return te;
}

/* ------------------------------------------------------ */

/* pretty print worker's output in human-readable terms */
/* te: elapsed time in seconds
 * mt: amount of transferred data in MiB
 * type: see 'worker' above
 *
 * return value: -
 */
void printout(double te, double mt, int type)
{
    switch (type)
    {
    case TEST_MEMCPY:
        printf("Method: MEMCPY\t");
        break;
    case TEST_DUMB:
        printf("Method: DUMB\t");
        break;
    case TEST_MCBLOCK:
        printf("Method: MCBLOCK\t");
        break;
    }
    printf("Elapsed: %.5f\t", te);
    printf("MiB: %.5f\t", mt);
    printf("Copy: %.3f MiB/s\n", mt / te);
    return;
}

/* ------------------------------------------------------ */

int main(int argc, char **argv)
{
    unsigned int long_size = 0;
    double te, te_sum;            /* time elapsed */
    unsigned long long asize = 0; /* array size (elements in array) */
    int i;
    int o; /* getopt options */
    unsigned long testno;

    /* options */

    /* how many runs to average? */
    int nr_loops = DEFAULT_NR_LOOPS;
    /* fixed memcpy block size for -t2 */
    unsigned long long block_size = DEFAULT_BLOCK_SIZE;
    /* show average, -a */
    int showavg = 1;
    /* what tests to run (-t x) */
    int tests[MAX_TESTS];
    double mt = 0; /* MiBytes transferred == array size in MiB */
    int quiet = 0; /* suppress extra messages */

    tests[0] = 0;
    tests[1] = 0;
    tests[2] = 1;

    while ((o = getopt(argc, argv, "haqn:t:b:p:")) != EOF)
    {
        switch (o)
        {
        case 'h':
            usage();
            exit(1);
            break;
        case 'a': /* suppress printing average */
            showavg = 0;
            break;
        case 'n': /* no. loops */
            nr_loops = strtoul(optarg, (char **)NULL, 10);
            break;
        case 't': /* test to run */
            testno = strtoul(optarg, (char **)NULL, 10);
            if (testno > MAX_TESTS - 1)
            {
                printf("Error: test number must be between 0 and %d\n", MAX_TESTS - 1);
                exit(1);
            }
            tests[testno] = 1;
            break;
        case 'b': /* block size in bytes*/
            block_size = strtoull(optarg, (char **)NULL, 10);
            if (0 >= block_size)
            {
                printf("Error: what block size do you mean?\n");
                exit(1);
            }
            break;
        case 'q': /* quiet */
            quiet = 1;
            break;
        case 'p':
            thread_num = strtoull(optarg, (char **)NULL, 10);
            break;

        default:
            break;
        }
    }

    /* default is to run all tests if no specific tests were requested */
    if ((tests[0] + tests[1] + tests[2]) == 0)
    {
        tests[0] = 1;
        tests[1] = 1;
        tests[2] = 1;
    }

    if (nr_loops == 0 && ((tests[0] + tests[1] + tests[2]) != 1))
    {
        printf("Error: nr_loops can be zero if only one test selected!\n");
        exit(1);
    }

    if (optind < argc)
    {
        mt = strtoul(argv[optind++], (char **)NULL, 10);
    }
    else
    {
        printf("Error: no array size given!\n");
        exit(1);
    }

    if (0 >= mt)
    {
        printf("Error: array size wrong!\n");
        exit(1);
    }

    /* ------------------------------------------------------ */

    long_size = sizeof(long);             /* the size of long on this platform */
    asize = 1024 * 1024 / long_size * mt; /* how many longs then in one array? */

    if (asize * long_size < block_size)
    {
        printf("Error: array size larger than block size (%llu bytes)!\n", block_size);
        exit(1);
    }

    if (!quiet)
    {
        printf("Long uses %d bytes. ", long_size);
        printf("Allocating 2*%lld elements = %lld bytes of memory.\n", asize, 2 * asize * long_size);
        if (tests[2])
        {
            printf("Using %lld bytes as blocks for memcpy block copy test.\n", block_size);
        }
    }

    /* ------------------------------------------------------ */
    if (!quiet)
    {
        printf("Getting down to business... Doing %d runs per test.\n", nr_loops);
    }

    for (int i = 0; i < thread_num; i++)
    {
        src_ptr.push_back(get_huge_mem(0, asize * sizeof(long)));
        dst_ptr.push_back(get_huge_mem(0, asize * sizeof(long)));
        spend_time.push_back(0);
    }
    long *tmp;
    long t;

    for (int i = 0; i < thread_num; i++)
    {
        tmp = (long *)src_ptr[i];
        for (t = 0; t < asize; t++)
        {
            tmp[t] = 0xaa;
        }
        tmp = (long *)dst_ptr[i];
        for (t = 0; t < asize; t++)
        {
            tmp[t] = 0xaa;
        }
    }

    /* run all tests requested, the proper number of times */
    for (testno = 0; testno < MAX_TESTS; testno++)
    {
        te_sum = 0;
        if (tests[testno])
        {
            for (i = 0; nr_loops == 0 || i < nr_loops; i++)
            {
                te = worker(asize, testno, block_size);
                te_sum += te;
                printf("%d\t", i);
                printout(te, mt, testno);
                for (int i = 0; i < thread_num; i++)
                {
                    printf("%f ", spend_time[i]);
                }
                printf("\n");
            }
            if (showavg)
            {
                printf("AVG\t");
                printout(te_sum / nr_loops, mt, testno);
            }
        }
    }

    return 0;
}
