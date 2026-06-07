#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PROCS 256
#define PID_LEN 32

typedef struct {
    char pid[PID_LEN];
    int arrival;
    int burst;
    int priority;
    int order;
} Process;

typedef struct {
    int pid_index; /* -1 means CPU idle */
    int start;
    int end;
} Segment;

typedef struct {
    Segment *items;
    int len;
    int cap;
} Timeline;

typedef struct {
    char algo[32];
    int quantum;
    Process procs[MAX_PROCS];
    int n;
    int start[MAX_PROCS];
    int finish[MAX_PROCS];
    int turnaround[MAX_PROCS];
    int waiting[MAX_PROCS];
    int response[MAX_PROCS];
    double avg_turnaround;
    double avg_waiting;
    double avg_response;
    double throughput;
    int context_switches;
    int total_time;
    Timeline timeline;
} Result;

typedef struct {
    int *data;
    int head;
    int tail;
    int cap;
} Queue;

static void die(const char *msg) {
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

static void die_errno(const char *msg) {
    fprintf(stderr, "error: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static void timeline_add(Timeline *tl, int pid_index, int start, int end) {
    if (end <= start) {
        return;
    }
    if (tl->len > 0) {
        Segment *prev = &tl->items[tl->len - 1];
        if (prev->pid_index == pid_index && prev->end == start) {
            prev->end = end;
            return;
        }
    }
    if (tl->len == tl->cap) {
        int next = tl->cap == 0 ? 32 : tl->cap * 2;
        Segment *grown = realloc(tl->items, (size_t)next * sizeof(*grown));
        if (!grown) {
            die_errno("realloc timeline");
        }
        tl->items = grown;
        tl->cap = next;
    }
    tl->items[tl->len++] = (Segment){pid_index, start, end};
}

static void queue_init(Queue *q, int cap) {
    q->cap = cap < 16 ? 16 : cap;
    q->data = malloc((size_t)q->cap * sizeof(*q->data));
    if (!q->data) {
        die_errno("malloc queue");
    }
    q->head = 0;
    q->tail = 0;
}

static int queue_empty(const Queue *q) {
    return q->head == q->tail;
}

static void queue_grow(Queue *q) {
    int count = q->tail - q->head;
    int next_cap = q->cap * 2;
    int *grown = malloc((size_t)next_cap * sizeof(*grown));
    if (!grown) {
        die_errno("grow queue");
    }
    for (int i = 0; i < count; ++i) {
        grown[i] = q->data[(q->head + i) % q->cap];
    }
    free(q->data);
    q->data = grown;
    q->cap = next_cap;
    q->head = 0;
    q->tail = count;
}

static void queue_push(Queue *q, int value) {
    if (q->tail - q->head >= q->cap - 1) {
        queue_grow(q);
    }
    q->data[q->tail % q->cap] = value;
    q->tail++;
}

static int queue_pop(Queue *q) {
    if (queue_empty(q)) {
        die("queue underflow");
    }
    int value = q->data[q->head % q->cap];
    q->head++;
    if (q->head > q->cap && q->head == q->tail) {
        q->head = 0;
        q->tail = 0;
    }
    return value;
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

static void read_processes(const char *path, Process procs[], int *n) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        die_errno(path);
    }

    char line[256];
    int line_no = 0;
    *n = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        trim(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        for (char *p = line; *p; ++p) {
            if (*p == ',' || *p == ';' || *p == '\t') {
                *p = ' ';
            }
        }
        Process p;
        int matched = sscanf(line, "%31s %d %d %d", p.pid, &p.arrival, &p.burst, &p.priority);
        if (matched != 4) {
            char lowered[PID_LEN];
            memset(lowered, 0, sizeof(lowered));
            for (size_t i = 0; i < strlen(line) && i < sizeof(lowered) - 1; ++i) {
                lowered[i] = (char)tolower((unsigned char)line[i]);
            }
            if (strstr(lowered, "pid") || strstr(lowered, "arrival")) {
                continue;
            }
            fprintf(stderr, "invalid input line %d: %s\n", line_no, line);
            exit(1);
        }
        if (p.arrival < 0 || p.burst <= 0) {
            fprintf(stderr, "invalid process at line %d: arrival must be >=0 and burst >0\n", line_no);
            exit(1);
        }
        if (*n >= MAX_PROCS) {
            die("too many processes");
        }
        p.order = *n;
        procs[(*n)++] = p;
    }
    fclose(fp);
    if (*n == 0) {
        die("no processes loaded");
    }
}

static void copy_processes(Result *r, const char *algo, const Process procs[], int n, int quantum) {
    memset(r, 0, sizeof(*r));
    snprintf(r->algo, sizeof(r->algo), "%s", algo);
    r->quantum = quantum;
    r->n = n;
    for (int i = 0; i < n; ++i) {
        r->procs[i] = procs[i];
        r->start[i] = -1;
        r->finish[i] = -1;
        r->response[i] = -1;
    }
}

static int earlier_process(const Process procs[], int a, int b) {
    if (procs[a].arrival != procs[b].arrival) {
        return procs[a].arrival < procs[b].arrival;
    }
    return procs[a].order < procs[b].order;
}

static void sort_by_arrival(const Process procs[], int order[], int n) {
    for (int i = 0; i < n; ++i) {
        order[i] = i;
    }
    for (int i = 1; i < n; ++i) {
        int v = order[i];
        int j = i - 1;
        while (j >= 0 && !earlier_process(procs, order[j], v)) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
}

static void finish_metrics(Result *r) {
    int first_arrival = INT_MAX;
    int last_finish = 0;
    double sum_ta = 0.0, sum_wait = 0.0, sum_resp = 0.0;

    for (int i = 0; i < r->n; ++i) {
        if (r->finish[i] < 0) {
            die("internal error: unfinished process");
        }
        r->turnaround[i] = r->finish[i] - r->procs[i].arrival;
        r->waiting[i] = r->turnaround[i] - r->procs[i].burst;
        r->response[i] = r->start[i] - r->procs[i].arrival;
        sum_ta += r->turnaround[i];
        sum_wait += r->waiting[i];
        sum_resp += r->response[i];
        if (r->procs[i].arrival < first_arrival) {
            first_arrival = r->procs[i].arrival;
        }
        if (r->finish[i] > last_finish) {
            last_finish = r->finish[i];
        }
    }
    r->avg_turnaround = sum_ta / r->n;
    r->avg_waiting = sum_wait / r->n;
    r->avg_response = sum_resp / r->n;
    r->total_time = last_finish;
    int elapsed = last_finish - first_arrival;
    r->throughput = elapsed > 0 ? (double)r->n / elapsed : 0.0;

    int active_segments = 0;
    for (int i = 0; i < r->timeline.len; ++i) {
        if (r->timeline.items[i].pid_index >= 0) {
            active_segments++;
        }
    }
    r->context_switches = active_segments > 0 ? active_segments - 1 : 0;
}

static void simulate_fcfs(Result *r) {
    int order[MAX_PROCS];
    int time = 0;
    sort_by_arrival(r->procs, order, r->n);
    for (int pos = 0; pos < r->n; ++pos) {
        int i = order[pos];
        if (time < r->procs[i].arrival) {
            timeline_add(&r->timeline, -1, time, r->procs[i].arrival);
            time = r->procs[i].arrival;
        }
        r->start[i] = time;
        timeline_add(&r->timeline, i, time, time + r->procs[i].burst);
        time += r->procs[i].burst;
        r->finish[i] = time;
    }
    finish_metrics(r);
}

static int choose_nonpreemptive(Result *r, const int done[], int time, const char *algo) {
    int best = -1;
    for (int i = 0; i < r->n; ++i) {
        if (done[i] || r->procs[i].arrival > time) {
            continue;
        }
        if (best < 0) {
            best = i;
            continue;
        }
        if (strcmp(algo, "sjf") == 0) {
            if (r->procs[i].burst < r->procs[best].burst ||
                (r->procs[i].burst == r->procs[best].burst && earlier_process(r->procs, i, best))) {
                best = i;
            }
        } else if (strcmp(algo, "priority") == 0) {
            if (r->procs[i].priority < r->procs[best].priority ||
                (r->procs[i].priority == r->procs[best].priority && earlier_process(r->procs, i, best))) {
                best = i;
            }
        } else if (strcmp(algo, "hrrn") == 0) {
            double wait_i = (double)(time - r->procs[i].arrival);
            double wait_b = (double)(time - r->procs[best].arrival);
            double ratio_i = (wait_i + r->procs[i].burst) / r->procs[i].burst;
            double ratio_b = (wait_b + r->procs[best].burst) / r->procs[best].burst;
            if (ratio_i > ratio_b ||
                (ratio_i == ratio_b && earlier_process(r->procs, i, best))) {
                best = i;
            }
        }
    }
    return best;
}

static int next_arrival_of_unfinished(Result *r, const int done[]) {
    int next = INT_MAX;
    for (int i = 0; i < r->n; ++i) {
        if (!done[i] && r->procs[i].arrival < next) {
            next = r->procs[i].arrival;
        }
    }
    return next;
}

static void simulate_nonpreemptive(Result *r, const char *algo) {
    int done[MAX_PROCS] = {0};
    int finished = 0;
    int time = 0;
    while (finished < r->n) {
        int chosen = choose_nonpreemptive(r, done, time, algo);
        if (chosen < 0) {
            int next = next_arrival_of_unfinished(r, done);
            timeline_add(&r->timeline, -1, time, next);
            time = next;
            continue;
        }
        r->start[chosen] = time;
        timeline_add(&r->timeline, chosen, time, time + r->procs[chosen].burst);
        time += r->procs[chosen].burst;
        r->finish[chosen] = time;
        done[chosen] = 1;
        finished++;
    }
    finish_metrics(r);
}

static int choose_preemptive(Result *r, const int remaining[], int time, const char *algo) {
    int best = -1;
    for (int i = 0; i < r->n; ++i) {
        if (remaining[i] <= 0 || r->procs[i].arrival > time) {
            continue;
        }
        if (best < 0) {
            best = i;
            continue;
        }
        if (strcmp(algo, "srtf") == 0) {
            if (remaining[i] < remaining[best] ||
                (remaining[i] == remaining[best] && earlier_process(r->procs, i, best))) {
                best = i;
            }
        } else if (strcmp(algo, "priorityp") == 0) {
            if (r->procs[i].priority < r->procs[best].priority ||
                (r->procs[i].priority == r->procs[best].priority && earlier_process(r->procs, i, best))) {
                best = i;
            }
        }
    }
    return best;
}

static int next_arrival_with_remaining(Result *r, const int remaining[]) {
    int next = INT_MAX;
    for (int i = 0; i < r->n; ++i) {
        if (remaining[i] > 0 && r->procs[i].arrival < next) {
            next = r->procs[i].arrival;
        }
    }
    return next;
}

static void simulate_preemptive(Result *r, const char *algo) {
    int remaining[MAX_PROCS];
    int finished = 0;
    int time = 0;
    for (int i = 0; i < r->n; ++i) {
        remaining[i] = r->procs[i].burst;
    }
    while (finished < r->n) {
        int chosen = choose_preemptive(r, remaining, time, algo);
        if (chosen < 0) {
            int next = next_arrival_with_remaining(r, remaining);
            timeline_add(&r->timeline, -1, time, next);
            time = next;
            continue;
        }
        if (r->start[chosen] < 0) {
            r->start[chosen] = time;
        }
        timeline_add(&r->timeline, chosen, time, time + 1);
        remaining[chosen]--;
        time++;
        if (remaining[chosen] == 0) {
            r->finish[chosen] = time;
            finished++;
        }
    }
    finish_metrics(r);
}

static void add_arrivals_until(Result *r, const int order[], int *next_pos, int time, Queue *q) {
    while (*next_pos < r->n && r->procs[order[*next_pos]].arrival <= time) {
        queue_push(q, order[*next_pos]);
        (*next_pos)++;
    }
}

static int adaptive_quantum(const int remaining[], int n, int fallback) {
    int count = 0;
    int sum = 0;
    for (int i = 0; i < n; ++i) {
        if (remaining[i] > 0) {
            sum += remaining[i];
            count++;
        }
    }
    if (count == 0) {
        return fallback;
    }
    int q = (sum + count - 1) / count;
    if (q < 1) {
        q = 1;
    }
    return q;
}

static void simulate_rr(Result *r, int adaptive) {
    int remaining[MAX_PROCS];
    int order[MAX_PROCS];
    int next_pos = 0;
    int finished = 0;
    int time = 0;
    Queue q;

    if (!adaptive && r->quantum <= 0) {
        die("RR quantum must be > 0");
    }
    for (int i = 0; i < r->n; ++i) {
        remaining[i] = r->procs[i].burst;
    }
    sort_by_arrival(r->procs, order, r->n);
    queue_init(&q, r->n * 4 + 16);

    while (finished < r->n) {
        add_arrivals_until(r, order, &next_pos, time, &q);
        if (queue_empty(&q)) {
            if (next_pos < r->n) {
                int next_time = r->procs[order[next_pos]].arrival;
                timeline_add(&r->timeline, -1, time, next_time);
                time = next_time;
                add_arrivals_until(r, order, &next_pos, time, &q);
            }
            continue;
        }

        int i = queue_pop(&q);
        if (remaining[i] <= 0) {
            continue;
        }
        if (r->start[i] < 0) {
            r->start[i] = time;
        }
        int qlen = adaptive ? adaptive_quantum(remaining, r->n, r->quantum > 0 ? r->quantum : 2) : r->quantum;
        int run = remaining[i] < qlen ? remaining[i] : qlen;
        timeline_add(&r->timeline, i, time, time + run);
        time += run;
        remaining[i] -= run;
        add_arrivals_until(r, order, &next_pos, time, &q);
        if (remaining[i] > 0) {
            queue_push(&q, i);
        } else {
            r->finish[i] = time;
            finished++;
        }
    }
    free(q.data);
    finish_metrics(r);
}

static void run_algorithm(Result *r, const char *algo, const Process procs[], int n, int quantum) {
    copy_processes(r, algo, procs, n, quantum);
    if (strcmp(algo, "fcfs") == 0) {
        simulate_fcfs(r);
    } else if (strcmp(algo, "sjf") == 0 || strcmp(algo, "priority") == 0 || strcmp(algo, "hrrn") == 0) {
        simulate_nonpreemptive(r, algo);
    } else if (strcmp(algo, "srtf") == 0 || strcmp(algo, "priorityp") == 0) {
        simulate_preemptive(r, algo);
    } else if (strcmp(algo, "rr") == 0) {
        simulate_rr(r, 0);
    } else if (strcmp(algo, "arr") == 0) {
        simulate_rr(r, 1);
    } else {
        fprintf(stderr, "unknown algorithm: %s\n", algo);
        exit(1);
    }
}

static const char *analysis_for(const char *algo) {
    if (strcmp(algo, "fcfs") == 0) {
        return "FCFS实现简单且公平按到达顺序执行，但短作业可能被长作业阻塞，平均等待时间容易偏高。";
    }
    if (strcmp(algo, "sjf") == 0) {
        return "SJF在已知服务时间时通常能降低平均等待/周转时间，但长作业在短作业持续到达时可能饥饿。";
    }
    if (strcmp(algo, "srtf") == 0) {
        return "SRTF是抢占式SJF，响应新到达短作业更快，但切换次数更多，并依赖准确的CPU burst估计。";
    }
    if (strcmp(algo, "rr") == 0) {
        return "RR通过时间片改善交互响应；时间片过小会增加上下文切换，过大则退化为FCFS。";
    }
    if (strcmp(algo, "priority") == 0) {
        return "非抢占优先级调度便于表达重要性，但低优先级进程可能长期等待，需要老化机制缓解。";
    }
    if (strcmp(algo, "priorityp") == 0) {
        return "抢占式优先级调度能让高优先级进程及时运行，但更容易造成低优先级饥饿。";
    }
    if (strcmp(algo, "hrrn") == 0) {
        return "HRRN按(等待时间+服务时间)/服务时间选择进程，在偏向短作业的同时用等待时间抑制饥饿。";
    }
    if (strcmp(algo, "arr") == 0) {
        return "自适应RR按剩余时间动态估计时间片，目标是在响应性和切换开销之间取得更稳的折中。";
    }
    return "";
}

static void print_result(const Result *r) {
    printf("ALGORITHM %s", r->algo);
    if (strcmp(r->algo, "rr") == 0) {
        printf(" quantum=%d", r->quantum);
    }
    if (strcmp(r->algo, "arr") == 0) {
        printf(" base_quantum=%d", r->quantum);
    }
    printf("\n");

    printf("GANTT\n");
    for (int i = 0; i < r->timeline.len; ++i) {
        const Segment *s = &r->timeline.items[i];
        const char *name = s->pid_index < 0 ? "IDLE" : r->procs[s->pid_index].pid;
        printf("  [%d,%d) %s\n", s->start, s->end, name);
    }

    printf("RESULTS\n");
    printf("pid arrival burst priority start finish turnaround waiting response\n");
    for (int i = 0; i < r->n; ++i) {
        printf("%s %d %d %d %d %d %d %d %d\n",
               r->procs[i].pid,
               r->procs[i].arrival,
               r->procs[i].burst,
               r->procs[i].priority,
               r->start[i],
               r->finish[i],
               r->turnaround[i],
               r->waiting[i],
               r->response[i]);
    }

    printf("SUMMARY\n");
    printf("avg_turnaround=%.3f\n", r->avg_turnaround);
    printf("avg_waiting=%.3f\n", r->avg_waiting);
    printf("avg_response=%.3f\n", r->avg_response);
    printf("throughput=%.3f jobs/unit\n", r->throughput);
    printf("context_switches=%d\n", r->context_switches);
    printf("ANALYSIS %s\n", analysis_for(r->algo));
}

static void free_result(Result *r) {
    free(r->timeline.items);
    r->timeline.items = NULL;
    r->timeline.len = 0;
    r->timeline.cap = 0;
}

static void print_compare(const Process procs[], int n, int quantum) {
    const char *algos[] = {"fcfs", "sjf", "srtf", "rr", "priority", "priorityp", "hrrn", "arr"};
    const int algo_count = (int)(sizeof(algos) / sizeof(algos[0]));
    Result results[8];
    int best_wait = 0;
    int best_turnaround = 0;

    printf("COMPARE algorithms=%d quantum=%d\n", algo_count, quantum);
    printf("algo avg_turnaround avg_waiting avg_response throughput context_switches\n");
    for (int i = 0; i < algo_count; ++i) {
        run_algorithm(&results[i], algos[i], procs, n, quantum);
        printf("%s %.3f %.3f %.3f %.3f %d\n",
               results[i].algo,
               results[i].avg_turnaround,
               results[i].avg_waiting,
               results[i].avg_response,
               results[i].throughput,
               results[i].context_switches);
        if (results[i].avg_waiting < results[best_wait].avg_waiting) {
            best_wait = i;
        }
        if (results[i].avg_turnaround < results[best_turnaround].avg_turnaround) {
            best_turnaround = i;
        }
    }
    printf("ANALYSIS best_avg_waiting=%s(%.3f) best_avg_turnaround=%s(%.3f)\n",
           results[best_wait].algo,
           results[best_wait].avg_waiting,
           results[best_turnaround].algo,
           results[best_turnaround].avg_turnaround);
    printf("ANALYSIS 抢占式算法通常改善响应时间但增加切换次数；SJF/SRTF依赖CPU时间预测；RR/ARR适合交互任务。\n");

    for (int i = 0; i < algo_count; ++i) {
        free_result(&results[i]);
    }
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  %s --input processes.csv --algo fcfs|sjf|srtf|rr|priority|priorityp|hrrn|arr [--quantum N]\n"
            "  %s --input processes.csv --compare [--quantum N]\n"
            "\n"
            "Input CSV columns: pid,arrival,burst,priority (smaller priority value means higher priority).\n",
            argv0, argv0);
}

int main(int argc, char **argv) {
    const char *input = NULL;
    const char *algo = "fcfs";
    int compare = 0;
    int quantum = 2;
    Process procs[MAX_PROCS];
    int n = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input = argv[++i];
        } else if (strcmp(argv[i], "--algo") == 0 && i + 1 < argc) {
            algo = argv[++i];
        } else if (strcmp(argv[i], "--quantum") == 0 && i + 1 < argc) {
            quantum = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--compare") == 0) {
            compare = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (!input) {
        usage(argv[0]);
        return 1;
    }
    if (quantum <= 0) {
        die("quantum must be > 0");
    }

    read_processes(input, procs, &n);
    if (compare) {
        print_compare(procs, n, quantum);
    } else {
        Result r;
        run_algorithm(&r, algo, procs, n, quantum);
        print_result(&r);
        free_result(&r);
    }
    return 0;
}
