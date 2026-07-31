/*
 * udpx.c - Linux UDP Network Probe (server + client)
 *
 * A UDP echo-based network diagnostic tool that measures round-trip time,
 * jitter, packet loss, reordering, and percentile latency statistics.
 * Build:  gcc -O2 -Wall -Wextra -o udpx udpx.c -lm
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <time.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */
#define DEFAULT_PORT            50505
#define DEFAULT_PAYLOAD_SIZE    64
#define MIN_PAYLOAD_SIZE        1
#define MAX_PAYLOAD_SIZE        65000      /* leave room for headers */
#define DEFAULT_COUNT           1000
#define RECV_TIMEOUT_SEC        2
#define SEND_TIMEOUT_SEC        2
#define SOCKET_BUFFER_SIZE      (256 * 1024)
#define MAX_RATE_PER_SEC        10000      /* server-side abuse guard */
#define STATS_DISPLAY_INTERVAL  10         /* seconds, continuous mode */
#define LIVE_UPDATE_INTERVAL    100        /* packets, live mode      */
#define RTT_HISTORY_INITIAL_CAP 4096       /* dynamic buffer start    */
#define PROGRESS_BAR_SEGMENTS   50

#define PACKET_MAGIC            0xABCD1234u
#define udpx_VERSION        "2.0.0-linux"

/* Run modes */
typedef enum {
    MODE_COUNT,      /* run for a specific packet count   */
    MODE_DURATION,   /* run for a specific time duration  */
    MODE_CONTINUOUS  /* run until interrupted             */
} run_mode_t;

/* ------------------------------------------------------------------ */
/* Wire format                                                         */
/* ------------------------------------------------------------------ */

/*
 * Packet header. Timestamps are carried as raw monotonic-clock ticks from
 * the *sender's* clock; the receiving side never interprets them itself,
 * it just echoes them back, so there is no cross-host clock assumption.
 */
typedef struct {
    unsigned int   magic;         /* validation marker           */
    unsigned int   seq;           /* sequence number              */
    unsigned int   payload_size;  /* size of payload data          */
    int64_t        ts_sec;        /* sender monotonic clock: sec  */
    int64_t        ts_nsec;       /* sender monotonic clock: nsec */
    unsigned int   crc32;         /* CRC32 over header+payload    */
} pkt_header_t;

typedef struct {
    pkt_header_t  header;
    unsigned char *payload;   /* heap-allocated, MAX_PAYLOAD_SIZE bytes */
} pkt_t;

/* ------------------------------------------------------------------ */
/* Global state for signal handling                                    */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t g_running = 1;
static int g_sock = -1;
static int g_is_tty = 0; /* whether stdout is a terminal (fancy output ok) */

/* ------------------------------------------------------------------ */
/* Statistics                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    unsigned long packets_sent;
    unsigned long packets_received;
    unsigned long packets_lost;
    unsigned long invalid_packets;
    unsigned long out_of_order;

    double min_rtt;
    double max_rtt;
    double sum_rtt;
    double sum_rtt_squared;

    /* jitter (RFC 3550 style: |delta rtt|) */
    double prev_rtt;
    double sum_jitter;
    double min_jitter;
    double max_jitter;
    unsigned long jitter_samples;

    /* RTT history, dynamically grown, for percentile calculations */
    double *rtt_history;
    unsigned long rtt_history_size;
    unsigned long rtt_history_capacity;

    /* inter-packet arrival timing */
    struct timespec last_recv_time;
    int has_last_recv_time;
    double sum_inter_packet_delay;
    unsigned long inter_packet_samples;
} stats_t;

/* ------------------------------------------------------------------ */
/* CRC32 (payload + header integrity)                                  */
/* ------------------------------------------------------------------ */
static unsigned int crc32_table[256];
static int crc32_table_ready = 0;

static void crc32_init(void) {
    for (unsigned int i = 0; i < 256; i++) {
        unsigned int c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

static unsigned int crc32_update(unsigned int crc, const unsigned char *buf, size_t len) {
    if (!crc32_table_ready) crc32_init();
    crc = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Compute checksum over header fields (excluding crc32 itself) + payload */
static unsigned int calc_checksum(const pkt_header_t *hdr, const unsigned char *payload) {
    unsigned int crc = 0;
    unsigned int fields[3] = { hdr->magic, hdr->seq, hdr->payload_size };
    crc = crc32_update(crc, (const unsigned char *)fields, sizeof(fields));
    crc = crc32_update(crc, (const unsigned char *)&hdr->ts_sec, sizeof(hdr->ts_sec));
    crc = crc32_update(crc, (const unsigned char *)&hdr->ts_nsec, sizeof(hdr->ts_nsec));
    if (hdr->payload_size > 0 && payload != NULL) {
        crc = crc32_update(crc, payload, hdr->payload_size);
    }
    return crc;
}

static int validate_packet(const pkt_header_t *hdr, const unsigned char *payload) {
    if (hdr->magic != PACKET_MAGIC) return 0;
    unsigned int expected = calc_checksum(hdr, payload);
    return (hdr->crc32 == expected);
}

static size_t get_packet_size(unsigned int payload_size) {
    return sizeof(pkt_header_t) + payload_size;
}

/* ------------------------------------------------------------------ */
/* Time helpers                                                         */
/* ------------------------------------------------------------------ */

/* Milliseconds between two monotonic timespecs (b - a is NOT taken; this
 * returns (later - earlier) in ms, caller decides order). */
static double ts_diff_ms(const struct timespec *later, const struct timespec *earlier) {
    double sec_diff  = (double)(later->tv_sec  - earlier->tv_sec);
    double nsec_diff = (double)(later->tv_nsec - earlier->tv_nsec);
    return sec_diff * 1000.0 + nsec_diff / 1e6;
}

static void ts_now(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

/* ------------------------------------------------------------------ */
/* Stats management                                                     */
/* ------------------------------------------------------------------ */
static void init_stats(stats_t *stats) {
    memset(stats, 0, sizeof(stats_t));
    stats->min_rtt = 1e9;
    stats->max_rtt = 0;
    stats->min_jitter = 1e9;
    stats->max_jitter = 0;
    stats->prev_rtt = -1.0;
    stats->has_last_recv_time = 0;

    stats->rtt_history_capacity = RTT_HISTORY_INITIAL_CAP;
    stats->rtt_history = (double *)malloc(stats->rtt_history_capacity * sizeof(double));
    if (stats->rtt_history == NULL) {
        fprintf(stderr, "Warning: could not allocate RTT history buffer\n");
        stats->rtt_history_capacity = 0;
    }
    stats->rtt_history_size = 0;
}

static void free_stats(stats_t *stats) {
    free(stats->rtt_history);
    stats->rtt_history = NULL;
}

/* Add an RTT sample, growing the history buffer (doubling) as needed. */
static void add_rtt_sample(stats_t *stats, double rtt) {
    if (stats->rtt_history_capacity == 0) return; /* allocation failed earlier */

    if (stats->rtt_history_size >= stats->rtt_history_capacity) {
        unsigned long new_cap = stats->rtt_history_capacity * 2;
        double *bigger = (double *)realloc(stats->rtt_history, new_cap * sizeof(double));
        if (bigger == NULL) {
            /* Keep existing data; stop collecting further percentile samples. */
            return;
        }
        stats->rtt_history = bigger;
        stats->rtt_history_capacity = new_cap;
    }
    stats->rtt_history[stats->rtt_history_size++] = rtt;
}

static void update_jitter(stats_t *stats, double current_rtt) {
    if (stats->prev_rtt >= 0) {
        double jitter = fabs(current_rtt - stats->prev_rtt);
        stats->sum_jitter += jitter;
        stats->jitter_samples++;
        if (jitter < stats->min_jitter) stats->min_jitter = jitter;
        if (jitter > stats->max_jitter) stats->max_jitter = jitter;
    }
    stats->prev_rtt = current_rtt;
}

static int compare_double(const void *a, const void *b) {
    double diff = *(const double *)a - *(const double *)b;
    return (diff > 0) - (diff < 0);
}

static double calculate_percentile(const double *sorted_array, unsigned long size, double percentile) {
    if (size == 0) return 0.0;
    if (size == 1) return sorted_array[0];
    double index = (percentile / 100.0) * (double)(size - 1);
    unsigned long lower = (unsigned long)index;
    unsigned long upper = lower + 1;
    if (upper >= size) return sorted_array[size - 1];
    double weight = index - (double)lower;
    return sorted_array[lower] * (1.0 - weight) + sorted_array[upper] * weight;
}

static double calculate_stddev(const stats_t *stats) {
    if (stats->packets_received < 2) return 0.0;
    double mean = stats->sum_rtt / (double)stats->packets_received;
    double variance = (stats->sum_rtt_squared / (double)stats->packets_received) - (mean * mean);
    return (variance > 0) ? sqrt(variance) : 0.0;
}

/* ------------------------------------------------------------------ */
/* Output helpers                                                        */
/* ------------------------------------------------------------------ */

static void print_interim_stats(const stats_t *stats, long elapsed_time) {
    double loss_pct = stats->packets_sent > 0 ?
        (double)stats->packets_lost / (double)stats->packets_sent * 100.0 : 0.0;
    double avg_rtt = stats->packets_received > 0 ?
        stats->sum_rtt / (double)stats->packets_received : 0.0;
    double avg_jitter = stats->jitter_samples > 0 ?
        stats->sum_jitter / (double)stats->jitter_samples : 0.0;

    if (!g_is_tty) {
        printf("[interim %lds] sent=%lu recv=%lu lost=%lu (%.2f%%) avg_rtt=%.3fms avg_jitter=%.3fms\n",
               elapsed_time, stats->packets_sent, stats->packets_received,
               stats->packets_lost, loss_pct, avg_rtt, avg_jitter);
        fflush(stdout);
        return;
    }

    printf("\n");
    printf("+---------------------------------------------------------------+\n");
    printf("| INTERIM STATISTICS - Elapsed: %6ld s                          |\n", elapsed_time);
    printf("+---------------------------------------------------------------+\n");
    printf("| Packets Sent      : %-10lu                                |\n", stats->packets_sent);
    printf("| Packets Received  : %-10lu                                |\n", stats->packets_received);
    printf("| Packets Lost      : %-10lu (%.2f%%)                        |\n", stats->packets_lost, loss_pct);
    if (stats->packets_received > 0) {
        printf("| RTT avg/min/max   : %.3f / %.3f / %.3f ms                 |\n",
               avg_rtt, stats->min_rtt, stats->max_rtt);
        if (stats->jitter_samples > 0) {
            printf("| Jitter avg        : %.3f ms                                 |\n", avg_jitter);
        }
    }
    printf("+---------------------------------------------------------------+\n");
    fflush(stdout);
}

static void print_live_stats(const stats_t *stats) {
    double loss_pct = stats->packets_sent > 0 ?
        (double)stats->packets_lost / (double)stats->packets_sent * 100.0 : 0.0;
    double avg_rtt = stats->packets_received > 0 ?
        stats->sum_rtt / (double)stats->packets_received : 0.0;
    double avg_jitter = stats->jitter_samples > 0 ?
        stats->sum_jitter / (double)stats->jitter_samples : 0.0;

    const char *status = loss_pct < 0.1 ? "EXCELLENT" :
                          loss_pct < 1.0 ? "GOOD"      :
                          loss_pct < 5.0 ? "DEGRADED"  : "POOR";

    if (g_is_tty) {
        printf("\r[%-9s] sent:%6lu recv:%6lu loss:%5.2f%% rtt:%7.3fms jitter:%7.3fms",
               status, stats->packets_sent, stats->packets_received, loss_pct, avg_rtt, avg_jitter);
    } else {
        printf("[live] status=%s sent=%lu recv=%lu loss=%.2f%% rtt=%.3fms jitter=%.3fms\n",
               status, stats->packets_sent, stats->packets_received, loss_pct, avg_rtt, avg_jitter);
    }
    fflush(stdout);
}

static void write_csv_log(FILE *log_file, const stats_t *stats, long elapsed_time,
                           const char *server_ip, int port, unsigned int payload_size) {
    if (log_file == NULL) return;
    double loss_pct = stats->packets_sent > 0 ?
        (double)stats->packets_lost / (double)stats->packets_sent * 100.0 : 0.0;
    double avg_rtt = stats->packets_received > 0 ?
        stats->sum_rtt / (double)stats->packets_received : 0.0;
    double stddev = calculate_stddev(stats);
    double avg_jitter = stats->jitter_samples > 0 ?
        stats->sum_jitter / (double)stats->jitter_samples : 0.0;

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_info);

    fprintf(log_file, "%s,%s,%d,%u,%ld,%lu,%lu,%lu,%lu,%lu,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
            timestamp, server_ip, port, payload_size, elapsed_time,
            stats->packets_sent, stats->packets_received, stats->packets_lost,
            stats->invalid_packets, stats->out_of_order, loss_pct,
            stats->min_rtt, avg_rtt, stats->max_rtt, stddev,
            avg_jitter, stats->min_jitter, stats->max_jitter);
    fflush(log_file);
}

static void write_detailed_log(FILE *log_file, const stats_t *stats, long elapsed_time) {
    if (log_file == NULL) return;
    double loss_pct = stats->packets_sent > 0 ?
        (double)stats->packets_lost / (double)stats->packets_sent * 100.0 : 0.0;
    double avg_rtt = stats->packets_received > 0 ?
        stats->sum_rtt / (double)stats->packets_received : 0.0;
    double avg_jitter = stats->jitter_samples > 0 ?
        stats->sum_jitter / (double)stats->jitter_samples : 0.0;

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_info);

    double pps = elapsed_time > 0 ? (double)stats->packets_sent / (double)elapsed_time : 0.0;
    double success_rate = stats->packets_sent > 0 ?
        (double)stats->packets_received / (double)stats->packets_sent * 100.0 : 0.0;
    double rtt_stddev = calculate_stddev(stats);

    double jitter_stddev = 0.0;
    if (stats->jitter_samples > 1) {
        jitter_stddev = (stats->max_jitter - stats->min_jitter) / 4.0; /* rough estimate */
    }

    double p99_rtt = stats->max_rtt;
    if (stats->rtt_history_size > 0) {
        double *sorted = malloc(stats->rtt_history_size * sizeof(double));
        if (sorted != NULL) {
            memcpy(sorted, stats->rtt_history, stats->rtt_history_size * sizeof(double));
            qsort(sorted, stats->rtt_history_size, sizeof(double), compare_double);
            p99_rtt = calculate_percentile(sorted, stats->rtt_history_size, 99.0);
            free(sorted);
        }
    }

    const char *status;
    if (loss_pct < 0.1 && avg_jitter < 1.0)       status = "EXCELLENT";
    else if (loss_pct < 1.0 && avg_jitter < 5.0)  status = "GOOD";
    else if (loss_pct < 5.0)                      status = "DEGRADED";
    else                                          status = "POOR";

    fprintf(log_file,
            "%-19s | %5lds | %-10s | %8lu | %8lu | %8lu | %6.2f%% | %7.2f%% | "
            "%8.3f | %8.3f | %8.3f | %8.3f | %8.3f | %8.3f | %8.3f | %8.3f | %8lu | %8.2f | %8.3f\n",
            timestamp, elapsed_time, status,
            stats->packets_sent, stats->packets_received, stats->packets_lost, loss_pct, success_rate,
            stats->min_rtt, avg_rtt, stats->max_rtt, rtt_stddev,
            avg_jitter, stats->min_jitter, stats->max_jitter, jitter_stddev,
            stats->out_of_order, pps, p99_rtt);
    fflush(log_file);
}

/* Write a single JSON summary (overwritten atomically-ish at exit) */
static void write_json_summary(const char *path, const stats_t *stats, long elapsed_time,
                                const char *server_ip, int port, unsigned int payload_size,
                                double p50, double p95, double p99) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "Warning: could not write JSON summary to %s: %s\n", path, strerror(errno));
        return;
    }
    double loss_pct = stats->packets_sent > 0 ?
        (double)stats->packets_lost / (double)stats->packets_sent * 100.0 : 0.0;
    double avg_rtt = stats->packets_received > 0 ?
        stats->sum_rtt / (double)stats->packets_received : 0.0;
    double avg_jitter = stats->jitter_samples > 0 ?
        stats->sum_jitter / (double)stats->jitter_samples : 0.0;
    double stddev = calculate_stddev(stats);

    fprintf(f,
        "{\n"
        "  \"server\": \"%s\",\n"
        "  \"port\": %d,\n"
        "  \"payload_size\": %u,\n"
        "  \"elapsed_seconds\": %ld,\n"
        "  \"packets_sent\": %lu,\n"
        "  \"packets_received\": %lu,\n"
        "  \"packets_lost\": %lu,\n"
        "  \"invalid_packets\": %lu,\n"
        "  \"out_of_order\": %lu,\n"
        "  \"loss_pct\": %.4f,\n"
        "  \"rtt_ms\": { \"min\": %.4f, \"avg\": %.4f, \"max\": %.4f, \"stddev\": %.4f, "
        "\"p50\": %.4f, \"p95\": %.4f, \"p99\": %.4f },\n"
        "  \"jitter_ms\": { \"min\": %.4f, \"avg\": %.4f, \"max\": %.4f, \"samples\": %lu }\n"
        "}\n",
        server_ip, port, payload_size, elapsed_time,
        stats->packets_sent, stats->packets_received, stats->packets_lost,
        stats->invalid_packets, stats->out_of_order, loss_pct,
        stats->min_rtt, avg_rtt, stats->max_rtt, stddev, p50, p95, p99,
        stats->min_jitter, avg_jitter, stats->max_jitter, stats->jitter_samples);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Control flow helpers                                                  */
/* ------------------------------------------------------------------ */
static int should_continue(run_mode_t mode, int count, int current_count,
                            time_t start_time, int duration) {
    if (!g_running) return 0;
    switch (mode) {
        case MODE_COUNT:      return current_count < count;
        case MODE_DURATION:   return (time(NULL) - start_time) < duration;
        case MODE_CONTINUOUS: return 1;
        default:              return 0;
    }
}

static void signal_handler(int signum) {
    (void)signum;
    g_running = 0;
    if (g_sock >= 0) {
        shutdown(g_sock, SHUT_RDWR);
    }
}

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static int configure_socket(int sock) {
    int opt = 1;
    struct timeval tv;
    int bufsize = SOCKET_BUFFER_SIZE;

    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) < 0) {
        perror("Warning: setsockopt SO_RCVBUF");
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize)) < 0) {
        perror("Warning: setsockopt SO_SNDBUF");
    }

    tv.tv_sec = RECV_TIMEOUT_SEC;
    tv.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("Warning: setsockopt SO_RCVTIMEO");
    }

    tv.tv_sec = SEND_TIMEOUT_SEC;
    tv.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        perror("Warning: setsockopt SO_SNDTIMEO");
    }

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Warning: setsockopt SO_REUSEADDR");
    }
    return 0;
}

/* Allocate a pkt_t with a heap payload buffer sized to MAX_PAYLOAD_SIZE. */
static int pkt_alloc(pkt_t *p) {
    p->payload = (unsigned char *)malloc(MAX_PAYLOAD_SIZE);
    if (p->payload == NULL) return -1;
    memset(&p->header, 0, sizeof(p->header));
    return 0;
}

static void pkt_free(pkt_t *p) {
    free(p->payload);
    p->payload = NULL;
}

/* Serialize header+payload into a flat send/recv buffer. */
static size_t pkt_serialize(const pkt_t *p, unsigned char *buf, size_t buf_len) {
    size_t total = get_packet_size(p->header.payload_size);
    if (total > buf_len) return 0;
    memcpy(buf, &p->header, sizeof(p->header));
    if (p->header.payload_size > 0) {
        memcpy(buf + sizeof(p->header), p->payload, p->header.payload_size);
    }
    return total;
}

/* Deserialize a flat buffer into header+payload (payload buffer must
 * already be allocated with capacity >= MAX_PAYLOAD_SIZE). */
static int pkt_deserialize(pkt_t *p, const unsigned char *buf, size_t len) {
    if (len < sizeof(pkt_header_t)) return -1;
    memcpy(&p->header, buf, sizeof(p->header));
    size_t expected = get_packet_size(p->header.payload_size);
    if (len != expected) return -1;
    if (p->header.payload_size > MAX_PAYLOAD_SIZE) return -1;
    if (p->header.payload_size > 0) {
        memcpy(p->payload, buf + sizeof(p->header), p->header.payload_size);
    }
    return 0;
}

/* Resolve a host (IPv4/IPv6 literal or hostname) + port into a sockaddr
 * usable for connect()/sendto(). Returns 0 on success. */
static int resolve_target(const char *host, int port, int force_family,
                           struct sockaddr_storage *out_addr, socklen_t *out_len, int *out_family) {
    struct addrinfo hints, *res = NULL, *rp;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = force_family; /* AF_UNSPEC, AF_INET, or AF_INET6 */
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "Error: could not resolve '%s': %s\n", host, gai_strerror(rc));
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        if (rp->ai_addrlen <= sizeof(*out_addr)) {
            memcpy(out_addr, rp->ai_addr, rp->ai_addrlen);
            *out_len = rp->ai_addrlen;
            *out_family = rp->ai_family;
            freeaddrinfo(res);
            return 0;
        }
    }
    freeaddrinfo(res);
    fprintf(stderr, "Error: no usable address found for '%s'\n", host);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Server                                                                */
/* ------------------------------------------------------------------ */
static void run_server(int port, int force_family) {
    int sock;
    struct sockaddr_storage bind_addr, cli;
    socklen_t cli_len;
    unsigned char *buf;
    ssize_t n;
    pkt_t pkt;
    unsigned long packet_count = 0;
    unsigned long invalid_count = 0;
    time_t last_rate_check = 0;
    unsigned long rate_counter = 0;

    printf("Starting Network Probe Server (Linux, v%s)...\n", udpx_VERSION);

    int family = (force_family == AF_INET6) ? AF_INET6 : AF_INET;
    sock = socket(family, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    g_sock = sock;

    if (configure_socket(sock) < 0) {
        close(sock);
        exit(EXIT_FAILURE);
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    if (family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&bind_addr;
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons((uint16_t)port);
        a6->sin6_addr = in6addr_any;
        if (bind(sock, (struct sockaddr *)a6, sizeof(*a6)) < 0) {
            perror("bind");
            close(sock);
            exit(EXIT_FAILURE);
        }
    } else {
        struct sockaddr_in *a4 = (struct sockaddr_in *)&bind_addr;
        a4->sin_family = AF_INET;
        a4->sin_port = htons((uint16_t)port);
        a4->sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, (struct sockaddr *)a4, sizeof(*a4)) < 0) {
            perror("bind");
            close(sock);
            exit(EXIT_FAILURE);
        }
    }

    if (pkt_alloc(&pkt) < 0) {
        fprintf(stderr, "Error: could not allocate packet buffer\n");
        close(sock);
        exit(EXIT_FAILURE);
    }
    buf = (unsigned char *)malloc(get_packet_size(MAX_PAYLOAD_SIZE));
    if (buf == NULL) {
        fprintf(stderr, "Error: could not allocate receive buffer\n");
        pkt_free(&pkt);
        close(sock);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on %s port %d (Ctrl+C to stop)\n",
           family == AF_INET6 ? "IPv6" : "IPv4", port);
    printf("Socket buffer size: %d KB\n", SOCKET_BUFFER_SIZE / 1024);
    printf("Supports variable packet sizes: %d - %d bytes\n", MIN_PAYLOAD_SIZE, MAX_PAYLOAD_SIZE);

    while (g_running) {
        cli_len = sizeof(cli);
        n = recvfrom(sock, buf, get_packet_size(MAX_PAYLOAD_SIZE), 0,
                     (struct sockaddr *)&cli, &cli_len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("recvfrom");
            break;
        }

        if (pkt_deserialize(&pkt, buf, (size_t)n) < 0) {
            invalid_count++;
            continue;
        }
        if (!validate_packet(&pkt.header, pkt.payload)) {
            invalid_count++;
            continue;
        }
        if (pkt.header.payload_size < MIN_PAYLOAD_SIZE ||
            pkt.header.payload_size > MAX_PAYLOAD_SIZE) {
            invalid_count++;
            continue;
        }

        /* Rate limiting */
        time_t now = time(NULL);
        if (now != last_rate_check) {
            last_rate_check = now;
            rate_counter = 0;
        }
        if (++rate_counter > MAX_RATE_PER_SEC) {
            fprintf(stderr, "Warning: rate limit exceeded, dropping packet\n");
            continue;
        }

        /* Echo back exactly what we received */
        if (sendto(sock, buf, (size_t)n, 0, (struct sockaddr *)&cli, cli_len) < 0) {
            if (errno != EINTR) perror("sendto");
        }

        packet_count++;
        if (packet_count % 1000 == 0) {
            printf("Processed %lu packets (%lu invalid)\n", packet_count, invalid_count);
        }
    }

    printf("\nShutting down server...\n");
    printf("Total packets processed: %lu\n", packet_count);
    printf("Invalid packets rejected: %lu\n", invalid_count);

    free(buf);
    pkt_free(&pkt);
    close(sock);
    g_sock = -1;
}

/* ------------------------------------------------------------------ */
/* Client                                                                */
/* ------------------------------------------------------------------ */
static void run_client(const char *server_host, int port, run_mode_t mode,
                        int count, int duration, unsigned int payload_size,
                        int live_mode, const char *log_filename,
                        const char *json_filename, int force_family,
                        double rate_pps) {
    int sock;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    int family;
    struct timespec send_ts, recv_ts;
    pkt_t send_pkt, recv_pkt;
    unsigned char *send_buf, *recv_buf;
    ssize_t n;
    stats_t stats;
    double rtt;
    int i = 0;
    unsigned int expected_seq = 0;
    size_t packet_size;
    time_t start_time, last_display_time, last_log_time;
    FILE *log_file = NULL;
    FILE *csv_file = NULL;
    struct timespec next_send_time;
    int pacing = (rate_pps > 0.0);
    double send_interval_ns = pacing ? (1e9 / rate_pps) : 0.0;

    if (payload_size < MIN_PAYLOAD_SIZE || payload_size > MAX_PAYLOAD_SIZE) {
        fprintf(stderr, "Error: payload size must be between %d and %d bytes\n",
                MIN_PAYLOAD_SIZE, MAX_PAYLOAD_SIZE);
        exit(EXIT_FAILURE);
    }
    packet_size = get_packet_size(payload_size);

    init_stats(&stats);

    printf("Starting Network Probe Client (Linux, v%s)...\n", udpx_VERSION);
    printf("Target: %s:%d\n", server_host, port);
    switch (mode) {
        case MODE_COUNT:
            printf("Mode: Packet count (%d packets)\n", count);
            break;
        case MODE_DURATION:
            printf("Mode: Time duration (%d seconds)\n", duration);
            break;
        case MODE_CONTINUOUS:
            printf("Mode: Continuous (press Ctrl+C to stop)\n");
            printf("Stats display interval: %d seconds\n", STATS_DISPLAY_INTERVAL);
            break;
    }
    printf("Payload size: %u bytes (total packet: %zu bytes)\n", payload_size, packet_size);
    if (live_mode) printf("Live mode: enabled (updates every %d packets)\n", LIVE_UPDATE_INTERVAL);
    if (pacing)    printf("Rate limit: %.2f packets/sec (paced sending)\n", rate_pps);

    if (log_filename != NULL) {
        char csv_path[512], detailed_path[512];
        snprintf(csv_path, sizeof(csv_path), "%s.csv", log_filename);
        csv_file = fopen(csv_path, "w");
        if (csv_file == NULL) {
            fprintf(stderr, "Warning: could not create CSV log file: %s (%s)\n", csv_path, strerror(errno));
        } else {
            fprintf(csv_file,
                "Timestamp,Server,Port,PayloadSize,ElapsedTime,PacketsSent,PacketsReceived,"
                "PacketsLost,InvalidPackets,OutOfOrder,LossPct,MinRTT,AvgRTT,MaxRTT,StdDev,"
                "AvgJitter,MinJitter,MaxJitter\n");
            fflush(csv_file);
            printf("CSV logging: %s\n", csv_path);
        }

        snprintf(detailed_path, sizeof(detailed_path), "%s.log", log_filename);
        log_file = fopen(detailed_path, "w");
        if (log_file == NULL) {
            fprintf(stderr, "Warning: could not create detailed log file: %s (%s)\n", detailed_path, strerror(errno));
        } else {
            time_t now = time(NULL);
            char nowbuf[64];
            struct tm tm_info;
            localtime_r(&now, &tm_info);
            strftime(nowbuf, sizeof(nowbuf), "%Y-%m-%d %H:%M:%S", &tm_info);
            fprintf(log_file, "Network Probe - Detailed Log\n");
            fprintf(log_file, "Server: %s:%d\n", server_host, port);
            fprintf(log_file, "Payload Size: %u bytes\n", payload_size);
            fprintf(log_file, "Started: %s\n", nowbuf);
            fprintf(log_file, "=====================================\n\n");
            fprintf(log_file,
                "%-19s | %6s | %-10s | %8s | %8s | %8s | %7s | %8s | %8s | %8s | %8s | %8s | %8s | %8s | %8s | %8s | %8s | %8s | %8s\n",
                "Timestamp", "Time", "Status", "Sent", "Recv", "Lost", "Loss%", "Success%",
                "MinRTT", "AvgRTT", "MaxRTT", "StdDev", "AvgJit", "MinJit", "MaxJit", "JitStd",
                "Reorder", "PPS", "P99");
            fprintf(log_file,
                "%-19s-+-%6s-+-%-10s-+-%8s-+-%8s-+-%8s-+-%7s-+-%8s-+-%8s-+-%8s-+-%8s-+-%8s-+-%8s-+-%8s-+-%8s-+-%8s-+-%8s-+-%8s-+-%8s\n",
                "-------------------", "------", "----------", "--------", "--------", "--------",
                "-------", "--------", "--------", "--------", "--------", "--------", "--------",
                "--------", "--------", "--------", "--------", "--------", "--------");
            fflush(log_file);
            printf("Detailed logging: %s\n", detailed_path);
        }
    }
    if (json_filename != NULL) {
        printf("JSON summary will be written to: %s\n", json_filename);
    }

    if (resolve_target(server_host, port, force_family, &addr, &addr_len, &family) < 0) {
        free_stats(&stats);
        exit(EXIT_FAILURE);
    }

    sock = socket(family, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        free_stats(&stats);
        exit(EXIT_FAILURE);
    }
    g_sock = sock;

    if (configure_socket(sock) < 0) {
        close(sock);
        free_stats(&stats);
        exit(EXIT_FAILURE);
    }

    /* connect() the UDP socket so recv() only returns datagrams from the
     * target, and so send()/recv() can be used directly. */
    if (connect(sock, (struct sockaddr *)&addr, addr_len) < 0) {
        perror("connect");
        close(sock);
        free_stats(&stats);
        exit(EXIT_FAILURE);
    }

    if (pkt_alloc(&send_pkt) < 0 || pkt_alloc(&recv_pkt) < 0) {
        fprintf(stderr, "Error: could not allocate packet buffers\n");
        close(sock);
        free_stats(&stats);
        exit(EXIT_FAILURE);
    }
    send_buf = (unsigned char *)malloc(packet_size);
    recv_buf = (unsigned char *)malloc(get_packet_size(MAX_PAYLOAD_SIZE));
    if (send_buf == NULL || recv_buf == NULL) {
        fprintf(stderr, "Error: could not allocate send/recv buffers\n");
        pkt_free(&send_pkt);
        pkt_free(&recv_pkt);
        close(sock);
        free_stats(&stats);
        exit(EXIT_FAILURE);
    }

    printf("\n");
    printf("==============================================================================\n");
    printf("                        NETWORK PROBE - STARTING TEST\n");
    printf("==============================================================================\n\n");

    start_time = time(NULL);
    last_display_time = start_time;
    last_log_time = start_time;
    ts_now(&next_send_time);

    int progress_step = 1;
    if (!live_mode && mode == MODE_COUNT) {
        progress_step = (count >= PROGRESS_BAR_SEGMENTS) ? (count / PROGRESS_BAR_SEGMENTS) : 1;
        if (g_is_tty) { printf("Progress: ["); fflush(stdout); }
    }

    while (should_continue(mode, count, i, start_time, duration)) {
        /* Optional client-side pacing */
        if (pacing) {
            struct timespec now_ts;
            ts_now(&now_ts);
            double wait_ms = ts_diff_ms(&next_send_time, &now_ts);
            if (wait_ms > 0) {
                struct timespec req;
                req.tv_sec  = (time_t)(wait_ms / 1000.0);
                req.tv_nsec = (long)(fmod(wait_ms, 1000.0) * 1e6);
                nanosleep(&req, NULL);
            }
            next_send_time.tv_nsec += (long)send_interval_ns;
            while (next_send_time.tv_nsec >= 1000000000L) {
                next_send_time.tv_nsec -= 1000000000L;
                next_send_time.tv_sec += 1;
            }
        }

        /* Build packet */
        send_pkt.header.magic = PACKET_MAGIC;
        send_pkt.header.seq = (unsigned int)i;
        send_pkt.header.payload_size = payload_size;
        ts_now(&send_ts);
        send_pkt.header.ts_sec = (int64_t)send_ts.tv_sec;
        send_pkt.header.ts_nsec = (int64_t)send_ts.tv_nsec;
        for (unsigned int j = 0; j < payload_size; j++) {
            send_pkt.payload[j] = (unsigned char)(j & 0xFF);
        }
        send_pkt.header.crc32 = calc_checksum(&send_pkt.header, send_pkt.payload);

        size_t to_send = pkt_serialize(&send_pkt, send_buf, packet_size);
        if (to_send == 0 || send(sock, send_buf, to_send, 0) < 0) {
            if (errno != EINTR) perror("send");
            stats.packets_lost++;
            i++;
            continue;
        }
        stats.packets_sent++;

        n = recv(sock, recv_buf, get_packet_size(MAX_PAYLOAD_SIZE), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "Timeout on packet %d\n", i);
            } else if (errno != EINTR) {
                perror("recv");
            }
            stats.packets_lost++;
            i++;
            continue;
        }

        if (pkt_deserialize(&recv_pkt, recv_buf, (size_t)n) < 0) {
            stats.invalid_packets++;
            stats.packets_lost++;
            i++;
            continue;
        }
        if (!validate_packet(&recv_pkt.header, recv_pkt.payload)) {
            stats.invalid_packets++;
            stats.packets_lost++;
            i++;
            continue;
        }
        if (recv_pkt.header.payload_size != payload_size) {
            fprintf(stderr, "Payload size mismatch: expected %u, received %u\n",
                    payload_size, recv_pkt.header.payload_size);
            stats.invalid_packets++;
            stats.packets_lost++;
            i++;
            continue;
        }
        if (recv_pkt.header.seq != send_pkt.header.seq) {
            fprintf(stderr, "Sequence mismatch: sent %u, received %u\n",
                    send_pkt.header.seq, recv_pkt.header.seq);
            stats.invalid_packets++;
            stats.packets_lost++;
            i++;
            continue;
        }
        if (recv_pkt.header.seq != expected_seq) {
            stats.out_of_order++;
        }
        expected_seq = recv_pkt.header.seq + 1;

        ts_now(&recv_ts);
        struct timespec sent_ts;
        sent_ts.tv_sec  = (time_t)recv_pkt.header.ts_sec;
        sent_ts.tv_nsec = (long)recv_pkt.header.ts_nsec;
        rtt = ts_diff_ms(&recv_ts, &sent_ts);
        if (rtt < 0) rtt = 0; /* guard against clock oddities */

        if (rtt < stats.min_rtt) stats.min_rtt = rtt;
        if (rtt > stats.max_rtt) stats.max_rtt = rtt;
        stats.sum_rtt += rtt;
        stats.sum_rtt_squared += rtt * rtt;
        stats.packets_received++;

        add_rtt_sample(&stats, rtt);
        update_jitter(&stats, rtt);

        if (stats.has_last_recv_time) {
            double inter_delay = ts_diff_ms(&recv_ts, &stats.last_recv_time);
            stats.sum_inter_packet_delay += inter_delay;
            stats.inter_packet_samples++;
        }
        stats.last_recv_time = recv_ts;
        stats.has_last_recv_time = 1;

        time_t current_time = time(NULL);
        if (log_filename != NULL && current_time > last_log_time) {
            if (csv_file != NULL) write_csv_log(csv_file, &stats, current_time - start_time,
                                                 server_host, port, payload_size);
            if (log_file != NULL) write_detailed_log(log_file, &stats, current_time - start_time);
            last_log_time = current_time;
        }

        if (live_mode) {
            if ((i + 1) % LIVE_UPDATE_INTERVAL == 0) print_live_stats(&stats);
        } else if (mode == MODE_CONTINUOUS) {
            if (current_time - last_display_time >= STATS_DISPLAY_INTERVAL) {
                print_interim_stats(&stats, current_time - start_time);
                last_display_time = current_time;
            }
        } else if (mode == MODE_COUNT && (i + 1) % progress_step == 0) {
            if (g_is_tty) { printf("#"); fflush(stdout); }
        }

        i++;
    }

    if (live_mode) {
        printf("\n");
    } else if (mode == MODE_COUNT && g_is_tty) {
        printf("] Complete!\n");
    }
    printf("\n");

    double p50 = 0, p95 = 0, p99 = 0;
    if (stats.rtt_history_size > 0) {
        qsort(stats.rtt_history, stats.rtt_history_size, sizeof(double), compare_double);
        p50 = calculate_percentile(stats.rtt_history, stats.rtt_history_size, 50.0);
        p95 = calculate_percentile(stats.rtt_history, stats.rtt_history_size, 95.0);
        p99 = calculate_percentile(stats.rtt_history, stats.rtt_history_size, 99.0);
    }

    double loss_pct = stats.packets_sent > 0 ?
        (double)stats.packets_lost / (double)stats.packets_sent * 100.0 : 0.0;
    double avg_rtt = stats.packets_received > 0 ?
        stats.sum_rtt / (double)stats.packets_received : 0.0;
    double stddev = calculate_stddev(&stats);
    double avg_jitter = stats.jitter_samples > 0 ?
        stats.sum_jitter / (double)stats.jitter_samples : 0.0;
    double avg_delay = stats.inter_packet_samples > 0 ?
        stats.sum_inter_packet_delay / (double)stats.inter_packet_samples : 0.0;

    const char *overall_status;
    if (loss_pct < 0.1 && avg_jitter < 1.0)        overall_status = "EXCELLENT - Network performing optimally";
    else if (loss_pct < 1.0 && avg_jitter < 5.0)   overall_status = "GOOD - Network within acceptable parameters";
    else if (loss_pct < 5.0)                       overall_status = "DEGRADED - Network showing signs of stress";
    else                                           overall_status = "POOR - Network requires immediate attention";

    printf("\n");
    printf("==============================================================================\n");
    printf("                        NETWORK PROBE - FINAL RESULTS\n");
    printf("==============================================================================\n");
    printf("Overall Status: %s\n", overall_status);
    printf("------------------------------------------------------------------------------\n");
    printf("PACKET STATISTICS\n");
    printf("  Packets Sent             : %lu\n", stats.packets_sent);
    printf("  Packets Received         : %lu\n", stats.packets_received);
    printf("  Packets Lost             : %lu (%.2f%%)\n", stats.packets_lost, loss_pct);
    printf("  Invalid Packets          : %lu\n", stats.invalid_packets);
    printf("  Out-of-Order Packets     : %lu\n", stats.out_of_order);

    if (stats.packets_received > 0) {
        printf("------------------------------------------------------------------------------\n");
        printf("ROUND-TRIP TIME (RTT) ANALYSIS\n");
        printf("  Minimum                  : %.3f ms\n", stats.min_rtt);
        printf("  Average                  : %.3f ms\n", avg_rtt);
        printf("  Maximum                  : %.3f ms\n", stats.max_rtt);
        printf("  Std Deviation            : %.3f ms\n", stddev);
        if (stats.rtt_history_size > 0) {
            printf("  Percentiles              : P50=%.3f ms  P95=%.3f ms  P99=%.3f ms\n", p50, p95, p99);
        }

        if (stats.jitter_samples > 0) {
            printf("------------------------------------------------------------------------------\n");
            printf("JITTER (LATENCY VARIATION) ANALYSIS\n");
            printf("  Minimum                  : %.3f ms\n", stats.min_jitter);
            printf("  Average                  : %.3f ms\n", avg_jitter);
            printf("  Maximum                  : %.3f ms\n", stats.max_jitter);
            printf("  Samples                  : %lu\n", stats.jitter_samples);
        }

        if (stats.inter_packet_samples > 0) {
            printf("------------------------------------------------------------------------------\n");
            printf("INTER-PACKET TIMING ANALYSIS\n");
            printf("  Average Inter-Packet Delay: %.3f ms\n", avg_delay);
            printf("  Samples Collected         : %lu\n", stats.inter_packet_samples);
        }

        printf("------------------------------------------------------------------------------\n");
        printf("NETWORK QUALITY ASSESSMENT\n");
        const char *loss_rating    = loss_pct   < 0.1 ? "Excellent" : loss_pct   < 1.0 ? "Good" : loss_pct   < 5.0  ? "Fair" : "Poor";
        const char *jitter_rating  = avg_jitter < 1.0 ? "Excellent" : avg_jitter < 5.0 ? "Good" : avg_jitter < 10.0 ? "Fair" : "Poor";
        const char *latency_rating = avg_rtt    < 1.0 ? "Excellent" : avg_rtt    < 10.0 ? "Good" : avg_rtt    < 50.0 ? "Fair" : "Poor";
        printf("  Packet Loss Rating       : %s\n", loss_rating);
        printf("  Jitter Rating            : %s\n", jitter_rating);
        printf("  Latency Rating           : %s\n", latency_rating);
    } else {
        printf("------------------------------------------------------------------------------\n");
        printf("WARNING: No valid responses received - check network connectivity\n");
    }
    printf("==============================================================================\n");

    if (json_filename != NULL) {
        write_json_summary(json_filename, &stats, time(NULL) - start_time,
                            server_host, port, payload_size, p50, p95, p99);
        printf("\nJSON summary written to: %s\n", json_filename);
    }

    if (csv_file != NULL) {
        fclose(csv_file);
        printf("CSV log file closed.\n");
    }
    if (log_file != NULL) {
        fprintf(log_file, "\n=== Test Completed ===\n");
        time_t end_time = time(NULL);
        char endbuf[64];
        struct tm tm_info;
        localtime_r(&end_time, &tm_info);
        strftime(endbuf, sizeof(endbuf), "%Y-%m-%d %H:%M:%S", &tm_info);
        fprintf(log_file, "End Time: %s\n", endbuf);
        fclose(log_file);
        printf("Detailed log file closed.\n");
    }

    pkt_free(&send_pkt);
    pkt_free(&recv_pkt);
    free(send_buf);
    free(recv_buf);
    free_stats(&stats);
    close(sock);
    g_sock = -1;
}

/* ------------------------------------------------------------------ */
/* Argument parsing helpers                                              */
/* ------------------------------------------------------------------ */

/* Parse a positive integer, exiting with an error message on failure. */
static long parse_long_arg(const char *arg, const char *name, long min_val, long max_val) {
    errno = 0;
    char *end = NULL;
    long val = strtol(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0') {
        fprintf(stderr, "Error: invalid value for %s: '%s'\n", name, arg);
        exit(EXIT_FAILURE);
    }
    if (val < min_val || val > max_val) {
        fprintf(stderr, "Error: %s must be between %ld and %ld (got %ld)\n", name, min_val, max_val, val);
        exit(EXIT_FAILURE);
    }
    return val;
}

static double parse_double_arg(const char *arg, const char *name, double min_val, double max_val) {
    errno = 0;
    char *end = NULL;
    double val = strtod(arg, &end);
    if (errno != 0 || end == arg || *end != '\0') {
        fprintf(stderr, "Error: invalid value for %s: '%s'\n", name, arg);
        exit(EXIT_FAILURE);
    }
    if (val < min_val || val > max_val) {
        fprintf(stderr, "Error: %s must be between %.2f and %.2f (got %.2f)\n", name, min_val, max_val, val);
        exit(EXIT_FAILURE);
    }
    return val;
}

static void print_usage(const char *prog) {
    printf("Network Probe - Linux Edition v%s\n", udpx_VERSION);
    printf("Usage:\n");
    printf("  Server: %s -s [-p port] [-4|-6]\n", prog);
    printf("  Client: %s -c <host> [options]\n", prog);
    printf("\nClient Options:\n");
    printf("  -p port          Port number (default: %d)\n", DEFAULT_PORT);
    printf("  --size bytes     Payload size in bytes (default: %d, range %d-%d)\n",
           DEFAULT_PAYLOAD_SIZE, MIN_PAYLOAD_SIZE, MAX_PAYLOAD_SIZE);
    printf("  --rate pps       Pace sending to at most this many packets/sec\n");
    printf("  --live           Enable live statistics display (updates every %d packets)\n",
           LIVE_UPDATE_INTERVAL);
    printf("  --log <file>     Enable logging (creates <file>.csv and <file>.log)\n");
    printf("  --json <file>    Write a JSON summary to <file> when the run finishes\n");
    printf("  -4 / -6          Force IPv4 or IPv6 resolution (default: auto)\n");
    printf("\nRun Mode (choose one):\n");
    printf("  -n count         Run for a specific packet count (default: %d)\n", DEFAULT_COUNT);
    printf("  -t seconds       Run for a specific duration in seconds\n");
    printf("  --continuous     Run continuously until Ctrl+C (stats every %ds)\n",
           STATS_DISPLAY_INTERVAL);
    printf("\nCommon Payload Sizes:\n");
    printf("  --size 1         Heartbeat/minimal (tests CPU interrupt capacity)\n");
    printf("  --size 64        Small control traffic (default)\n");
    printf("  --size 1500      MTU-level test (tests network throughput)\n");
    printf("  --size 8192      Large packet test\n");
    printf("\nMetrics: RTT (monotonic clock), jitter, percentiles (P50/P95/P99),\n");
    printf("         packet reordering, inter-packet delay, standard deviation.\n");
    printf("\nExamples:\n");
    printf("  %s -c 192.168.1.100                       # 1000 packets, 64 bytes\n", prog);
    printf("  %s -c example.com -n 5000                 # hostname, 5000 packets\n", prog);
    printf("  %s -c 192.168.1.100 -t 60                 # run for 60 seconds\n", prog);
    printf("  %s -c 192.168.1.100 --continuous          # run until Ctrl+C\n", prog);
    printf("  %s -c 192.168.1.100 --size 1500 -t 30     # MTU test for 30 seconds\n", prog);
    printf("  %s -c 192.168.1.100 -n 5000 --rate 200    # paced at 200 pps\n", prog);
    printf("  %s -c ::1 -6 -n 100                       # IPv6 loopback\n", prog);
}

/* ------------------------------------------------------------------ */
/* Main                                                                  */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    int count = DEFAULT_COUNT;
    int duration = 0;
    unsigned int payload_size = DEFAULT_PAYLOAD_SIZE;
    run_mode_t mode = MODE_COUNT;
    int live_mode = 0;
    char *log_filename = NULL;
    char *json_filename = NULL;
    int force_family = AF_UNSPEC;
    double rate_pps = 0.0;

    g_is_tty = isatty(STDOUT_FILENO);

    setup_signals();
    crc32_init();

    if (argc < 2 ||
        strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        exit(EXIT_SUCCESS);
    }
    if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
        printf("udpx %s\n", udpx_VERSION);
        exit(EXIT_SUCCESS);
    }

    int is_server = (strcmp(argv[1], "-s") == 0);
    int is_client = (strcmp(argv[1], "-c") == 0 && argc >= 3);

    if (!is_server && !is_client) {
        fprintf(stderr, "Error: invalid arguments\n");
        fprintf(stderr, "Use '%s --help' for usage\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int start_idx = is_client ? 3 : 2;
    int mode_count = 0;

    for (int i = start_idx; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = (int)parse_long_arg(argv[++i], "-p", 1, 65535);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            mode = MODE_COUNT;
            mode_count++;
            count = (int)parse_long_arg(argv[++i], "-n", 1, 1000000);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            mode = MODE_DURATION;
            mode_count++;
            duration = (int)parse_long_arg(argv[++i], "-t", 1, 86400);
        } else if (strcmp(argv[i], "--continuous") == 0) {
            mode = MODE_CONTINUOUS;
            mode_count++;
        } else if (strcmp(argv[i], "--live") == 0) {
            live_mode = 1;
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            payload_size = (unsigned int)parse_long_arg(argv[++i], "--size", MIN_PAYLOAD_SIZE, MAX_PAYLOAD_SIZE);
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_filename = argv[++i];
        } else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            json_filename = argv[++i];
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            rate_pps = parse_double_arg(argv[++i], "--rate", 0.001, 1000000.0);
        } else if (strcmp(argv[i], "-4") == 0) {
            force_family = AF_INET;
        } else if (strcmp(argv[i], "-6") == 0) {
            force_family = AF_INET6;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        } else {
            fprintf(stderr, "Error: unrecognized option '%s'\n", argv[i]);
            fprintf(stderr, "Use '%s --help' for usage\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    if (mode_count > 1) {
        fprintf(stderr, "Error: cannot specify multiple run modes (-n, -t, --continuous)\n");
        exit(EXIT_FAILURE);
    }

    if (is_server) {
        run_server(port, force_family);
    } else {
        run_client(argv[2], port, mode, count, duration, payload_size,
                   live_mode, log_filename, json_filename, force_family, rate_pps);
    }

    return EXIT_SUCCESS;
}
