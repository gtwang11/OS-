#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define MAX_PROCS 256
#define PID_LEN 32

typedef struct {
    char pid[PID_LEN];
    int arrival;
    int burst;
    int priority;
    int order;
} Proc;

typedef struct {
    char name[32];
    double avg_turnaround;
    double avg_waiting;
    double avg_response;
    double throughput;
    int context_switches;
} Metric;

static void die(const char *msg) {
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int opt_int(int argc, char **argv, const char *name, int fallback) {
    for (int i = 2; i + 1 < argc; ++i) {
        if (strcmp(argv[i], name) == 0) {
            return atoi(argv[i + 1]);
        }
    }
    return fallback;
}

static const char *opt_str(int argc, char **argv, const char *name, const char *fallback) {
    for (int i = 2; i + 1 < argc; ++i) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return fallback;
}

static void trim(char *s) {
    char *p = s;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void load_procs(const char *path, Proc procs[], int *n) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "error: %s: %s\n", path, strerror(errno));
        exit(1);
    }
    char line[256];
    int line_no = 0;
    *n = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        trim(line);
        if (!line[0] || line[0] == '#') {
            continue;
        }
        for (char *p = line; *p; ++p) {
            if (*p == ',' || *p == ';' || *p == '\t') {
                *p = ' ';
            }
        }
        Proc p;
        int matched = sscanf(line, "%31s %d %d %d", p.pid, &p.arrival, &p.burst, &p.priority);
        if (matched != 4) {
            char lowered[128] = {0};
            for (size_t i = 0; i < strlen(line) && i < sizeof(lowered) - 1; ++i) {
                lowered[i] = (char)tolower((unsigned char)line[i]);
            }
            if (strstr(lowered, "pid") || strstr(lowered, "arrival")) {
                continue;
            }
            fprintf(stderr, "invalid process line %d: %s\n", line_no, line);
            exit(1);
        }
        if (p.arrival < 0 || p.burst <= 0) {
            die("arrival must be >=0 and burst >0");
        }
        p.order = *n;
        procs[(*n)++] = p;
        if (*n >= MAX_PROCS) {
            die("too many processes");
        }
    }
    fclose(fp);
    if (*n == 0) {
        die("no processes loaded");
    }
}

static int earlier(const Proc p[], int a, int b) {
    if (p[a].arrival != p[b].arrival) {
        return p[a].arrival < p[b].arrival;
    }
    return p[a].order < p[b].order;
}

static void sort_arrival(const Proc p[], int order[], int n) {
    for (int i = 0; i < n; ++i) {
        order[i] = i;
    }
    for (int i = 1; i < n; ++i) {
        int v = order[i];
        int j = i - 1;
        while (j >= 0 && !earlier(p, order[j], v)) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
}

static void finalize_metric(Metric *m, const char *name, const Proc p[], int n, const int start[], const int finish[], int switches) {
    double sum_ta = 0, sum_wait = 0, sum_resp = 0;
    int first = p[0].arrival;
    int last = 0;
    for (int i = 0; i < n; ++i) {
        int ta = finish[i] - p[i].arrival;
        int wait = ta - p[i].burst;
        int resp = start[i] - p[i].arrival;
        sum_ta += ta;
        sum_wait += wait;
        sum_resp += resp;
        if (p[i].arrival < first) {
            first = p[i].arrival;
        }
        if (finish[i] > last) {
            last = finish[i];
        }
    }
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->avg_turnaround = sum_ta / n;
    m->avg_waiting = sum_wait / n;
    m->avg_response = sum_resp / n;
    m->throughput = last > first ? (double)n / (last - first) : 0.0;
    m->context_switches = switches;
}

static void metric_fcfs(Metric *m, const Proc p[], int n) {
    int order[MAX_PROCS], start[MAX_PROCS], finish[MAX_PROCS];
    int time = 0;
    int switches = 0;
    sort_arrival(p, order, n);
    for (int k = 0; k < n; ++k) {
        int i = order[k];
        if (time < p[i].arrival) {
            time = p[i].arrival;
        }
        if (k > 0) {
            switches++;
        }
        start[i] = time;
        time += p[i].burst;
        finish[i] = time;
    }
    finalize_metric(m, "fcfs", p, n, start, finish, switches);
}

static int next_unfinished_arrival(const Proc p[], int n, const int done[]) {
    int next = 1 << 30;
    for (int i = 0; i < n; ++i) {
        if (!done[i] && p[i].arrival < next) {
            next = p[i].arrival;
        }
    }
    return next;
}

static void metric_sjf_hrrn(Metric *m, const Proc p[], int n, int hrrn) {
    int done[MAX_PROCS] = {0}, start[MAX_PROCS], finish[MAX_PROCS];
    int completed = 0, time = 0, switches = 0;
    while (completed < n) {
        int best = -1;
        for (int i = 0; i < n; ++i) {
            if (done[i] || p[i].arrival > time) {
                continue;
            }
            if (best < 0) {
                best = i;
            } else if (!hrrn) {
                if (p[i].burst < p[best].burst || (p[i].burst == p[best].burst && earlier(p, i, best))) {
                    best = i;
                }
            } else {
                double ri = (double)(time - p[i].arrival + p[i].burst) / p[i].burst;
                double rb = (double)(time - p[best].arrival + p[best].burst) / p[best].burst;
                if (ri > rb || (ri == rb && earlier(p, i, best))) {
                    best = i;
                }
            }
        }
        if (best < 0) {
            time = next_unfinished_arrival(p, n, done);
            continue;
        }
        if (completed > 0) {
            switches++;
        }
        start[best] = time;
        time += p[best].burst;
        finish[best] = time;
        done[best] = 1;
        completed++;
    }
    finalize_metric(m, hrrn ? "hrrn_optimized" : "sjf", p, n, start, finish, switches);
}

typedef struct {
    int data[8192];
    int head;
    int tail;
} IntQueue;

static void qpush(IntQueue *q, int value) {
    if (q->tail >= (int)(sizeof(q->data) / sizeof(q->data[0]))) {
        die("queue overflow in RR benchmark");
    }
    q->data[q->tail++] = value;
}

static int qpop(IntQueue *q) {
    return q->data[q->head++];
}

static int qempty(const IntQueue *q) {
    return q->head == q->tail;
}

static int rr_quantum(const int remaining[], int n, int base, int adaptive) {
    if (!adaptive) {
        return base;
    }
    int count = 0, sum = 0;
    for (int i = 0; i < n; ++i) {
        if (remaining[i] > 0) {
            sum += remaining[i];
            count++;
        }
    }
    if (count == 0) {
        return base;
    }
    int q = (sum + count - 1) / count;
    return q < 1 ? 1 : q;
}

static void metric_rr(Metric *m, const Proc p[], int n, int base_quantum, int adaptive) {
    int order[MAX_PROCS], remaining[MAX_PROCS], start[MAX_PROCS], finish[MAX_PROCS];
    int next = 0, time = 0, complete = 0, switches = 0, segments = 0;
    IntQueue q = {{0}, 0, 0};
    for (int i = 0; i < n; ++i) {
        remaining[i] = p[i].burst;
        start[i] = -1;
    }
    sort_arrival(p, order, n);
    while (complete < n) {
        while (next < n && p[order[next]].arrival <= time) {
            qpush(&q, order[next++]);
        }
        if (qempty(&q)) {
            if (next < n) {
                time = p[order[next]].arrival;
                continue;
            }
            break;
        }
        int i = qpop(&q);
        if (start[i] < 0) {
            start[i] = time;
        }
        if (segments++ > 0) {
            switches++;
        }
        int qlen = rr_quantum(remaining, n, base_quantum, adaptive);
        int run = remaining[i] < qlen ? remaining[i] : qlen;
        time += run;
        remaining[i] -= run;
        while (next < n && p[order[next]].arrival <= time) {
            qpush(&q, order[next++]);
        }
        if (remaining[i] > 0) {
            qpush(&q, i);
        } else {
            finish[i] = time;
            complete++;
        }
    }
    finalize_metric(m, adaptive ? "adaptive_rr_optimized" : "rr", p, n, start, finish, switches);
}

static int run_optimize(int argc, char **argv) {
    const char *input = opt_str(argc, argv, "--input", "../01_scheduling/tests/processes.csv");
    int quantum = opt_int(argc, argv, "--quantum", 2);
    Proc p[MAX_PROCS];
    Metric m[5];
    int n = 0;
    load_procs(input, p, &n);
    metric_fcfs(&m[0], p, n);
    metric_sjf_hrrn(&m[1], p, n, 0);
    metric_rr(&m[2], p, n, quantum, 0);
    metric_sjf_hrrn(&m[3], p, n, 1);
    metric_rr(&m[4], p, n, quantum, 1);

    int best_wait = 0;
    printf("SCHED_OPTIMIZATION input=%s quantum=%d jobs=%d\n", input, quantum, n);
    printf("algo avg_turnaround avg_waiting avg_response throughput context_switches\n");
    for (int i = 0; i < 5; ++i) {
        printf("%s %.3f %.3f %.3f %.3f %d\n",
               m[i].name, m[i].avg_turnaround, m[i].avg_waiting, m[i].avg_response,
               m[i].throughput, m[i].context_switches);
        if (m[i].avg_waiting < m[best_wait].avg_waiting) {
            best_wait = i;
        }
    }
    printf("ANALYSIS optimized_algorithms=hrrn_optimized,adaptive_rr_optimized best_avg_waiting=%s %.3f\n",
           m[best_wait].name, m[best_wait].avg_waiting);
    printf("PASS scheduling_optimization_metrics_complete\n");
    return 0;
}

static int policy_from_name(const char *name) {
    if (strcmp(name, "fifo") == 0) {
        return SCHED_FIFO;
    }
    if (strcmp(name, "rr") == 0) {
        return SCHED_RR;
    }
    if (strcmp(name, "other") == 0) {
        return SCHED_OTHER;
    }
    die("policy must be fifo, rr, or other");
    return SCHED_OTHER;
}

static const char *policy_name(int policy) {
    switch (policy) {
    case SCHED_FIFO:
        return "SCHED_FIFO";
    case SCHED_RR:
        return "SCHED_RR";
    case SCHED_OTHER:
        return "SCHED_OTHER";
    default:
        return "UNKNOWN";
    }
}

static int run_rt(int argc, char **argv) {
    const char *policy_arg = opt_str(argc, argv, "--policy", "rr");
    int duration_ms = opt_int(argc, argv, "--duration-ms", 80);
    int policy = policy_from_name(policy_arg);
    int old_policy = sched_getscheduler(0);
    struct sched_param old_param;
    struct sched_param param;
    struct timespec interval;
    long long end_ns;
    volatile unsigned long long spin = 0;

    if (sched_getparam(0, &old_param) != 0) {
        memset(&old_param, 0, sizeof(old_param));
    }
    memset(&param, 0, sizeof(param));
    if (policy == SCHED_FIFO || policy == SCHED_RR) {
        param.sched_priority = sched_get_priority_min(policy);
    }
    printf("REALTIME policy=%s priority_range=[%d,%d] requested_priority=%d\n",
           policy_name(policy), sched_get_priority_min(policy), sched_get_priority_max(policy), param.sched_priority);
    if (policy == SCHED_RR && sched_rr_get_interval(0, &interval) == 0) {
        printf("RR_INTERVAL current_sec=%ld current_nsec=%ld\n", interval.tv_sec, interval.tv_nsec);
    }
    if (sched_setscheduler(0, policy, &param) != 0) {
        printf("REALTIME_APPLY result=SKIP_PERMISSION_OR_POLICY errno=%d message=%s\n", errno, strerror(errno));
    } else {
        printf("REALTIME_APPLY result=APPLIED active_policy=%s\n", policy_name(sched_getscheduler(0)));
    }

    end_ns = now_ns() + (long long)duration_ms * 1000000LL;
    while (now_ns() < end_ns) {
        spin = spin * 1664525ULL + 1013904223ULL;
    }
    printf("REALTIME_WORK duration_ms=%d spin=%llu\n", duration_ms, spin);

    if (sched_getscheduler(0) == policy && old_policy >= 0) {
        (void)sched_setscheduler(0, old_policy, &old_param);
    }
    printf("PASS realtime_policy_probe_complete\n");
    return 0;
}

typedef struct {
    long long iters;
    unsigned long long result;
} CpuArg;

static void *cpu_worker(void *arg) {
    CpuArg *a = arg;
    unsigned long long x = 1469598103934665603ULL;
    for (long long i = 0; i < a->iters; ++i) {
        x ^= (unsigned long long)i + 0x9e3779b97f4a7c15ULL;
        x *= 1099511628211ULL;
    }
    a->result = x;
    return NULL;
}

static double timeval_seconds(struct timeval tv) {
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static int run_perf(int argc, char **argv) {
    int threads = opt_int(argc, argv, "--threads", 4);
    long long iters = opt_int(argc, argv, "--iters", 300000);
    if (threads <= 0 || iters <= 0) {
        die("threads and iters must be positive");
    }
    pthread_t *tid = calloc((size_t)threads, sizeof(*tid));
    CpuArg *args = calloc((size_t)threads, sizeof(*args));
    if (!tid || !args) {
        die("calloc failed");
    }
    struct rusage before, after;
    getrusage(RUSAGE_SELF, &before);
    long long start = now_ns();
    for (int i = 0; i < threads; ++i) {
        args[i].iters = iters;
        pthread_create(&tid[i], NULL, cpu_worker, &args[i]);
    }
    unsigned long long checksum = 0;
    for (int i = 0; i < threads; ++i) {
        pthread_join(tid[i], NULL);
        checksum ^= args[i].result;
    }
    long long end = now_ns();
    getrusage(RUSAGE_SELF, &after);
    double wall = (double)(end - start) / 1000000000.0;
    double user = timeval_seconds(after.ru_utime) - timeval_seconds(before.ru_utime);
    double sys = timeval_seconds(after.ru_stime) - timeval_seconds(before.ru_stime);
    long vcsw = after.ru_nvcsw - before.ru_nvcsw;
    long ivcsw = after.ru_nivcsw - before.ru_nivcsw;
    double throughput = (double)threads * (double)iters / wall;
    printf("SYSTEM_PERF threads=%d iters_per_thread=%lld wall_sec=%.6f user_sec=%.6f sys_sec=%.6f throughput_iters_per_sec=%.0f checksum=%llu\n",
           threads, iters, wall, user, sys, throughput, checksum);
    printf("CONTEXT_SWITCH voluntary=%ld involuntary=%ld\n", vcsw, ivcsw);
    printf("ANALYSIS wall/user/sys时间和上下文切换数可用于判断CPU负载、线程并行度与调度开销。\n");
    printf("PASS system_performance_probe_complete\n");
    free(tid);
    free(args);
    return 0;
}

typedef struct {
    int id;
    long long iters;
    long long *shards;
} CounterArg;

static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static long long mutex_counter;
static long long atomic_counter;

static void *counter_mutex_worker(void *arg) {
    CounterArg *a = arg;
    for (long long i = 0; i < a->iters; ++i) {
        pthread_mutex_lock(&counter_mutex);
        mutex_counter++;
        pthread_mutex_unlock(&counter_mutex);
    }
    return NULL;
}

static void *counter_atomic_worker(void *arg) {
    CounterArg *a = arg;
    for (long long i = 0; i < a->iters; ++i) {
        __sync_fetch_and_add(&atomic_counter, 1);
    }
    return NULL;
}

static void *counter_shard_worker(void *arg) {
    CounterArg *a = arg;
    long long local = 0;
    for (long long i = 0; i < a->iters; ++i) {
        local++;
    }
    a->shards[a->id] = local;
    return NULL;
}

static double run_counter_case(int threads, void *(*fn)(void *), CounterArg args[], pthread_t tid[]) {
    long long start = now_ns();
    for (int i = 0; i < threads; ++i) {
        pthread_create(&tid[i], NULL, fn, &args[i]);
    }
    for (int i = 0; i < threads; ++i) {
        pthread_join(tid[i], NULL);
    }
    return (double)(now_ns() - start) / 1000000000.0;
}

static int run_concurrency(int argc, char **argv) {
    int threads = opt_int(argc, argv, "--threads", 4);
    long long iters = opt_int(argc, argv, "--iters", 100000);
    if (threads <= 0 || iters <= 0) {
        die("threads and iters must be positive");
    }
    pthread_t *tid = calloc((size_t)threads, sizeof(*tid));
    CounterArg *args = calloc((size_t)threads, sizeof(*args));
    long long *shards = calloc((size_t)threads, sizeof(*shards));
    if (!tid || !args || !shards) {
        die("calloc failed");
    }
    for (int i = 0; i < threads; ++i) {
        args[i] = (CounterArg){i, iters, shards};
    }
    long long expected = (long long)threads * iters;

    mutex_counter = 0;
    double t_mutex = run_counter_case(threads, counter_mutex_worker, args, tid);
    atomic_counter = 0;
    double t_atomic = run_counter_case(threads, counter_atomic_worker, args, tid);
    memset(shards, 0, (size_t)threads * sizeof(*shards));
    double t_shard = run_counter_case(threads, counter_shard_worker, args, tid);
    long long shard_total = 0;
    for (int i = 0; i < threads; ++i) {
        shard_total += shards[i];
    }

    printf("CONCURRENCY_OPT threads=%d iters_per_thread=%lld expected=%lld\n", threads, iters, expected);
    printf("strategy total seconds throughput_ops_per_sec\n");
    printf("mutex %lld %.6f %.0f\n", mutex_counter, t_mutex, (double)expected / t_mutex);
    printf("atomic %lld %.6f %.0f\n", atomic_counter, t_atomic, (double)expected / t_atomic);
    printf("sharded_local_reduce %lld %.6f %.0f\n", shard_total, t_shard, (double)expected / t_shard);
    const char *best = "mutex";
    double best_t = t_mutex;
    if (t_atomic < best_t) {
        best = "atomic";
        best_t = t_atomic;
    }
    if (t_shard < best_t) {
        best = "sharded_local_reduce";
        best_t = t_shard;
    }
    int pass = mutex_counter == expected && atomic_counter == expected && shard_total == expected;
    printf("ANALYSIS best_time_strategy=%s seconds=%.6f; 分片局部累加通过减少共享写竞争优化并发性能。\n", best, best_t);
    printf("SUMMARY result=%s\n", pass ? "PASS" : "FAIL");
    free(tid);
    free(args);
    free(shards);
    return pass ? 0 : 1;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  %s optimize --input processes.csv [--quantum N]\n"
            "  %s rt --policy rr|fifo|other [--duration-ms N]\n"
            "  %s perf --threads N --iters N\n"
            "  %s concurrency --threads N --iters N\n",
            argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "optimize") == 0) {
        return run_optimize(argc, argv);
    }
    if (strcmp(argv[1], "rt") == 0) {
        return run_rt(argc, argv);
    }
    if (strcmp(argv[1], "perf") == 0) {
        return run_perf(argc, argv);
    }
    if (strcmp(argv[1], "concurrency") == 0) {
        return run_concurrency(argc, argv);
    }
    usage(argv[0]);
    return 1;
}
