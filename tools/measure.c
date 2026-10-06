/* tools/measure.c — tiny process launcher for benchmarks.
 *
 * Lives under tools/ and is built with the system compiler on purpose: the
 * product is freestanding, but a measuring harness needs fork/wait4/getrusage
 * and a pty. Reports wall time and the child's peak RSS.
 *
 * usage:
 *   tools/measure [--runs N] -- CMD [ARGS...]
 *   tools/measure [--runs N] --pty -- CMD [ARGS...]   (sends "/quit" after 0.5 s)
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

static int one(int argc, char **argv, int pty) {
    double t0 = now_us();
    int master = -1, slave = -1;
    if (pty) {
        if (openpty(&master, &slave, NULL, NULL, NULL) != 0) return 1;
    }
    pid_t p = fork();
    if (p == 0) {
        if (pty) {
            setsid();
            dup2(slave, 0);
            dup2(slave, 1);
            dup2(slave, 2);
            close(master);
            close(slave);
        } else {
            int devnull = open("/dev/null", O_RDWR);
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    if (pty) {
        close(slave);
        usleep(500 * 1000);
        static const char quit[] = "/quit\n";
        if (write(master, quit, sizeof quit - 1) < 0) { /* child may have exited */ }
    }
    int status;
    struct rusage ru;
    wait4(p, &status, 0, &ru);
    double dt = now_us() - t0;
    if (pty) close(master);
    printf("%.0f us  rss %.2f MB  exit %d\n", dt, ru.ru_maxrss / 1024.0,
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return 0;
}

int main(int argc, char **argv) {
    int runs = 1, pty = 0, i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--runs") && i + 1 < argc) runs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pty")) pty = 1;
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: measure [--runs N] [--pty] -- CMD [ARGS...]\n");
        return 2;
    }
    double best = 0, sum = 0;
    for (int r = 0; r < runs; r++) {
        double t0 = now_us();
        one(argc - i, argv + i, pty);
        double dt = now_us() - t0;
        sum += dt;
        if (best == 0 || dt < best) best = dt;
    }
    if (runs > 1)
        fprintf(stderr, "runs=%d  mean %.0f us  best %.0f us\n", runs, sum / runs, best);
    return 0;
}
