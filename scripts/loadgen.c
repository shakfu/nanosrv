// Minimal threaded HTTP/1.1 keep-alive load generator.
// One thread per connection; each loops send-request / read-response and
// counts completed responses. Written because no load tool (wrk, ab, hey) is
// installed, and a Python client would contend for the very GIL under test.
//
// usage: loadgen <host> <port> <connections> <seconds> [path]
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile int g_stop = 0;
static const char *g_host, *g_path;
static int g_port;

// Latency samples are kept by reservoir sampling (Vitter's algorithm R): a
// uniform sample of the whole run in bounded memory, rather than the first N
// (which would be all warm-up) or the last N (which would miss a slow start).
#define SAMPLE_CAP 20000

struct result {
    long count;
    long errors;
    double lat_sum;
    double samples[SAMPLE_CAP];
    long nsamples;
    unsigned seed;
};

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int connect_once(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_port);
    sa.sin_addr.s_addr = inet_addr(g_host);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }
    return fd;
}

// Read one complete response: headers, then Content-Length bytes of body.
static int read_response(int fd, char *buf, size_t cap) {
    size_t total = 0;
    char *hdr_end = NULL;
    while (!hdr_end) {
        if (total >= cap - 1) return -1;
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n <= 0) return -1;
        total += (size_t)n;
        buf[total] = '\0';
        hdr_end = strstr(buf, "\r\n\r\n");
    }
    long clen = 0;
    char *cl = strcasestr(buf, "Content-Length:");
    if (cl && cl < hdr_end) clen = strtol(cl + 15, NULL, 10);
    size_t need = (size_t)(hdr_end - buf) + 4 + (size_t)clen;
    while (total < need) {
        if (total >= cap - 1) return -1;
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static void record(struct result *r, double latency) {
    r->lat_sum += latency;
    if (r->nsamples < SAMPLE_CAP) {
        r->samples[r->nsamples++] = latency;
    } else {
        // Replace an existing sample with probability SAMPLE_CAP/count.
        long j = (long)((double)rand_r(&r->seed) / ((double)RAND_MAX + 1.0)
                        * (double)(r->count + 1));
        if (j < SAMPLE_CAP) r->samples[j] = latency;
    }
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void *worker(void *arg) {
    struct result *r = (struct result *)arg;
    char req[512];
    int reqlen = snprintf(req, sizeof(req),
                          "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n",
                          g_path, g_host);
    char buf[65536];
    int fd = connect_once();
    while (!g_stop) {
        if (fd < 0) { fd = connect_once(); if (fd < 0) { r->errors++; usleep(1000); continue; } }
        double t0 = now_s();
        if (send(fd, req, (size_t)reqlen, MSG_NOSIGNAL) != reqlen) {
            close(fd); fd = -1; r->errors++; continue;
        }
        if (read_response(fd, buf, sizeof(buf)) != 0) {
            close(fd); fd = -1; r->errors++; continue;
        }
        record(r, now_s() - t0);
        r->count++;
    }
    if (fd >= 0) close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s host port conns seconds [path]\n", argv[0]); return 2; }
    g_host = argv[1];
    g_port = atoi(argv[2]);
    int conns = atoi(argv[3]);
    double secs = atof(argv[4]);
    g_path = argc > 5 ? argv[5] : "/";

    pthread_t *th = calloc((size_t)conns, sizeof(pthread_t));
    struct result *res = calloc((size_t)conns, sizeof(struct result));
    if (!th || !res) { fprintf(stderr, "out of memory\n"); return 1; }
    for (int i = 0; i < conns; i++) res[i].seed = (unsigned)(i * 2654435761u + 1u);
    double t0 = now_s();
    for (int i = 0; i < conns; i++) pthread_create(&th[i], NULL, worker, &res[i]);
    usleep((useconds_t)(secs * 1e6));
    g_stop = 1;
    for (int i = 0; i < conns; i++) pthread_join(th[i], NULL);
    double elapsed = now_s() - t0;

    long total = 0, errors = 0, nsamp = 0; double lat = 0;
    for (int i = 0; i < conns; i++) {
        total += res[i].count; errors += res[i].errors; lat += res[i].lat_sum;
        nsamp += res[i].nsamples;
    }

    double *all = nsamp ? malloc((size_t)nsamp * sizeof(double)) : NULL;
    long k = 0;
    if (all) {
        for (int i = 0; i < conns; i++)
            for (long j = 0; j < res[i].nsamples; j++) all[k++] = res[i].samples[j];
        qsort(all, (size_t)k, sizeof(double), cmp_double);
    }
    double p50 = k ? all[(long)(0.50 * (double)(k - 1))] : 0.0;
    double p99 = k ? all[(long)(0.99 * (double)(k - 1))] : 0.0;

    printf("{\"requests\": %ld, \"errors\": %ld, \"seconds\": %.3f, \"rps\": %.1f, "
           "\"mean_latency_us\": %.1f, \"p50_latency_us\": %.1f, "
           "\"p99_latency_us\": %.1f, \"samples\": %ld}\n",
           total, errors, elapsed, (double)total / elapsed,
           total ? lat / (double)total * 1e6 : 0.0, p50 * 1e6, p99 * 1e6, k);
    free(all); free(th); free(res);
    return 0;
}
