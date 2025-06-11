#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/userfaultfd.h>
#include <time.h>

void *map;
int uffd;
volatile int g_running = 1;

static void *uffd_handler_thread_fn(void *arg)
{
    static struct uffd_msg msg;
    struct uffdio_continue uffdio_continue_args;

    while (g_running) {
        struct pollfd pollfd = { .fd = uffd, .events = POLLIN };
        int nready = poll(&pollfd, 1, 200);
        if (nready < 0) {
            if (!g_running && errno == EBADF)
		    break;
            perror("poll() failed");
            exit(1);
        }
        if (nready == 0) continue;
        ssize_t nread = read(uffd, &msg, sizeof(msg));
        if (nread <= 0) {
            if (!g_running) break;
            fprintf(stderr, "read() from userfaultfd failed: %zd\n", nread);
            continue;
        }
        if (msg.event != UFFD_EVENT_PAGEFAULT) {
            fprintf(stderr, "Unexpected event on userfaultfd: %u\n", msg.event);
            continue;
        }
        uffdio_continue_args.range.start = msg.arg.pagefault.address;
        uffdio_continue_args.range.len = getpagesize();
        uffdio_continue_args.mode = 0;
        if (ioctl(uffd, UFFDIO_CONTINUE, &uffdio_continue_args) < 0) {
            perror("ioctl(UFFDIO_CONTINUE) failed");
            exit(1);
        }
    }
    return NULL;
}

static int setup_uffd(void)
{
    struct uffdio_api uffdio_api;
    struct uffdio_register uffdio_register;

    uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (uffd < 0) {
        perror("syscall(__NR_userfaultfd)");
        return -1;
    }

    uffdio_api.api = UFFD_API;
    uffdio_api.features = UFFD_FEATURE_MINOR_SHMEM;
    if (ioctl(uffd, UFFDIO_API, &uffdio_api) < 0) {
        perror("ioctl(UFFDIO_API)");
        return -1;
    }
    if (!(uffdio_api.features & UFFD_FEATURE_MINOR_SHMEM)) {
        fprintf(stderr, "UFFD_FEATURE_MINOR_SHMEM missing\n");
        return -1;
    }

    uffdio_register.range.start = (unsigned long)map;
    uffdio_register.range.len = getpagesize();
    uffdio_register.mode = UFFDIO_REGISTER_MODE_MINOR;
    if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) < 0) {
        perror("ioctl(UFFDIO_REGISTER)");
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    long long iterations;
    int fd;
    pthread_t uffd_thread;
    volatile int tmp = 0;
    struct timespec start_time, end_time;

    if (argc != 2) {
        fprintf(stderr, "uso: %s <iterations>\n", argv[0]);
        return 1;
    }
    iterations = atoll(argv[1]);
    if (iterations <= 0) {
        fprintf(stderr, "iteracoes nao podem ser negativas.\n");
        return 1;
    }

    fd = memfd_create("benchmark_shmem", 0);
    if (fd < 0) {
        perror("erro em memfd_create()");
        return 1;
    }
    if (ftruncate(fd, getpagesize()) < 0) {
        perror("erro em ftruncate()");
        return 1;
    }

    map = mmap(NULL, getpagesize(), PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        perror("erro em mmap()");
        return 1;
    }
    tmp += *(int *)map;
    if (setup_uffd() != 0) {
        fprintf(stderr, "erro no setup do userfaultfd\n");
        return 1;
    }
    if (pthread_create(&uffd_thread, NULL, uffd_handler_thread_fn, NULL) != 0) {
        perror("erro em pthread_create");
        return 1;
    }

    printf("[+] benchmark de %lld iteracoes...\n", iterations);
    
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (long long i = 0; i < iterations; i++) {
        if (madvise(map, getpagesize(), MADV_DONTNEED) < 0) {
            perror("erro em madvise() no loop");
            break;
        }
        tmp += *(volatile int *)map;
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);

    g_running = 0;
    close(uffd);
    pthread_join(uffd_thread, NULL);
    close(fd);

    double elapsed_ns = (end_time.tv_sec - start_time.tv_sec) * 1e9 +
                        (end_time.tv_nsec - start_time.tv_nsec);
    double ns_per_op = elapsed_ns / iterations;

    printf("[+] total de iteracoes: %lld\n", iterations);
    printf("[+] tempo total: %.4f seconds\n", elapsed_ns / 1e9);
    printf("[+] tempo medio por operacao: %.2f ns\n", ns_per_op);
    
    // ignore
    if (tmp == 0x12345678) printf("aaaaa\n");

    return 0;
}
