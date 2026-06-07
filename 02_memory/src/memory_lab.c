#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SEGMENTS 256
#define MAX_NAME 32
#define MAX_REFS 4096
#define MAX_FRAMES 128

typedef struct {
    int start;
    int size;
    int free;
    char name[MAX_NAME];
} Segment;

typedef struct {
    Segment segs[MAX_SEGMENTS];
    int count;
    int total_size;
    int allocations;
    int frees;
    int failures;
} Memory;

static void die(const char *msg) {
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

static void die_errno(const char *msg) {
    fprintf(stderr, "error: %s: %s\n", msg, strerror(errno));
    exit(1);
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

static void memory_init(Memory *m, int total) {
    memset(m, 0, sizeof(*m));
    m->total_size = total;
    m->count = 1;
    m->segs[0] = (Segment){0, total, 1, "FREE"};
}

static void print_memory(const Memory *m) {
    printf("MEMORY");
    for (int i = 0; i < m->count; ++i) {
        const Segment *s = &m->segs[i];
        printf(" | [%d,%d) %s:%s size=%d",
               s->start, s->start + s->size, s->free ? "FREE" : "ALLOC", s->name, s->size);
    }
    printf("\n");
}

static int segment_of_name(const Memory *m, const char *name) {
    for (int i = 0; i < m->count; ++i) {
        if (!m->segs[i].free && strcmp(m->segs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int choose_segment(const Memory *m, const char *algo, int size) {
    int best = -1;
    for (int i = 0; i < m->count; ++i) {
        if (!m->segs[i].free || m->segs[i].size < size) {
            continue;
        }
        if (best < 0) {
            best = i;
        } else if (strcmp(algo, "ff") == 0) {
            return best;
        } else if (strcmp(algo, "bf") == 0) {
            if (m->segs[i].size < m->segs[best].size) {
                best = i;
            }
        } else if (strcmp(algo, "wf") == 0) {
            if (m->segs[i].size > m->segs[best].size) {
                best = i;
            }
        } else {
            die("unknown partition algorithm; use ff, bf, or wf");
        }
    }
    return best;
}

static void split_segment(Memory *m, int idx, int size, const char *name) {
    if (m->segs[idx].size == size) {
        m->segs[idx].free = 0;
        snprintf(m->segs[idx].name, sizeof(m->segs[idx].name), "%s", name);
        return;
    }
    if (m->count >= MAX_SEGMENTS) {
        die("too many memory segments");
    }
    for (int i = m->count; i > idx + 1; --i) {
        m->segs[i] = m->segs[i - 1];
    }
    Segment old = m->segs[idx];
    m->segs[idx].size = size;
    m->segs[idx].free = 0;
    snprintf(m->segs[idx].name, sizeof(m->segs[idx].name), "%s", name);
    m->segs[idx + 1].start = old.start + size;
    m->segs[idx + 1].size = old.size - size;
    m->segs[idx + 1].free = 1;
    snprintf(m->segs[idx + 1].name, sizeof(m->segs[idx + 1].name), "FREE");
    m->count++;
}

static void coalesce(Memory *m) {
    for (int i = 0; i < m->count - 1;) {
        if (m->segs[i].free && m->segs[i + 1].free) {
            m->segs[i].size += m->segs[i + 1].size;
            for (int j = i + 1; j < m->count - 1; ++j) {
                m->segs[j] = m->segs[j + 1];
            }
            m->count--;
        } else {
            i++;
        }
    }
}

static void memory_alloc(Memory *m, const char *algo, const char *name, int size, int step) {
    if (size <= 0) {
        die("allocation size must be > 0");
    }
    if (segment_of_name(m, name) >= 0) {
        printf("STEP %d alloc %s %d -> FAIL duplicate_name\n", step, name, size);
        m->failures++;
        print_memory(m);
        return;
    }
    int idx = choose_segment(m, algo, size);
    if (idx < 0) {
        printf("STEP %d alloc %s %d -> FAIL no_suitable_hole\n", step, name, size);
        m->failures++;
        print_memory(m);
        return;
    }
    int start = m->segs[idx].start;
    split_segment(m, idx, size, name);
    m->allocations++;
    printf("STEP %d alloc %s %d -> OK start=%d\n", step, name, size, start);
    print_memory(m);
}

static void memory_free(Memory *m, const char *name, int step) {
    int idx = segment_of_name(m, name);
    if (idx < 0) {
        printf("STEP %d free %s -> FAIL not_found\n", step, name);
        m->failures++;
        print_memory(m);
        return;
    }
    m->segs[idx].free = 1;
    snprintf(m->segs[idx].name, sizeof(m->segs[idx].name), "FREE");
    coalesce(m);
    m->frees++;
    printf("STEP %d free %s -> OK\n", step, name);
    print_memory(m);
}

static void print_partition_stats(const Memory *m) {
    int total_free = 0;
    int largest_free = 0;
    int free_blocks = 0;
    for (int i = 0; i < m->count; ++i) {
        if (m->segs[i].free) {
            total_free += m->segs[i].size;
            free_blocks++;
            if (m->segs[i].size > largest_free) {
                largest_free = m->segs[i].size;
            }
        }
    }
    int external = total_free - largest_free;
    printf("SUMMARY total=%d allocations=%d frees=%d failures=%d free=%d largest_free=%d external_fragmentation=%d free_blocks=%d\n",
           m->total_size, m->allocations, m->frees, m->failures, total_free, largest_free, external, free_blocks);
    printf("ANALYSIS FF速度快但碎片位置依赖请求顺序；BF减少单次剩余空间但可能制造小碎片；WF保留中等孔洞但会消耗最大空闲块。\n");
}

static void run_partition(const char *algo, const char *path, int total_override) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        die_errno(path);
    }
    Memory mem;
    int initialized = 0;
    int step = 1;
    char line[256];

    if (total_override > 0) {
        memory_init(&mem, total_override);
        initialized = 1;
        printf("INIT total=%d algo=%s\n", total_override, algo);
        print_memory(&mem);
    }

    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        for (char *p = line; *p; ++p) {
            if (*p == ',' || *p == ';' || *p == '\t') {
                *p = ' ';
            }
        }
        char op[32], name[MAX_NAME];
        int size;
        if (sscanf(line, "%31s %d", op, &size) == 2 && strcmp(op, "total") == 0) {
            if (!initialized) {
                memory_init(&mem, size);
                initialized = 1;
                printf("INIT total=%d algo=%s\n", size, algo);
                print_memory(&mem);
            }
            continue;
        }
        if (!initialized) {
            die("partition trace must start with total N or use --size N");
        }
        if (sscanf(line, "%31s %31s %d", op, name, &size) == 3 && strcmp(op, "alloc") == 0) {
            memory_alloc(&mem, algo, name, size, step++);
        } else if (sscanf(line, "%31s %31s", op, name) == 2 && strcmp(op, "free") == 0) {
            memory_free(&mem, name, step++);
        } else if (strcmp(line, "dump") == 0) {
            printf("STEP %d dump\n", step++);
            print_memory(&mem);
        } else {
            fprintf(stderr, "invalid partition command: %s\n", line);
            exit(1);
        }
    }
    fclose(fp);
    if (!initialized) {
        die("no partition memory size provided");
    }
    print_partition_stats(&mem);
}

typedef struct {
    int page;
    int age;
    int loaded_at;
    int ref_bit;
} Frame;

static void parse_refs_from_file(const char *path, int *frames, int refs[], int *ref_count) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        die_errno(path);
    }
    char token[64];
    *ref_count = 0;
    while (fscanf(fp, "%63s", token) == 1) {
        if (token[0] == '#') {
            char rest[512];
            if (!fgets(rest, sizeof(rest), fp)) {
                break;
            }
            continue;
        }
        if (strcmp(token, "frames") == 0) {
            if (fscanf(fp, "%d", frames) != 1) {
                die("invalid frames line");
            }
        } else if (strcmp(token, "refs") == 0) {
            continue;
        } else {
            if (*ref_count >= MAX_REFS) {
                die("too many page references");
            }
            refs[(*ref_count)++] = atoi(token);
        }
    }
    fclose(fp);
}

static int parse_refs_string(const char *s, int refs[]) {
    int count = 0;
    char *copy = strdup(s);
    if (!copy) {
        die_errno("strdup refs");
    }
    for (char *p = copy; *p; ++p) {
        if (*p == ',' || *p == ';') {
            *p = ' ';
        }
    }
    char *tok = strtok(copy, " ");
    while (tok) {
        if (count >= MAX_REFS) {
            die("too many page references");
        }
        refs[count++] = atoi(tok);
        tok = strtok(NULL, " ");
    }
    free(copy);
    return count;
}

static int find_page(Frame frames[], int frame_count, int page) {
    for (int i = 0; i < frame_count; ++i) {
        if (frames[i].page == page) {
            return i;
        }
    }
    return -1;
}

static int empty_frame(Frame frames[], int frame_count) {
    for (int i = 0; i < frame_count; ++i) {
        if (frames[i].page < 0) {
            return i;
        }
    }
    return -1;
}

static int choose_victim(const char *algo, Frame frames[], int frame_count, const int refs[], int ref_count, int pos, int *clock_hand) {
    int victim = 0;
    if (strcmp(algo, "fifo") == 0) {
        for (int i = 1; i < frame_count; ++i) {
            if (frames[i].loaded_at < frames[victim].loaded_at) {
                victim = i;
            }
        }
    } else if (strcmp(algo, "lru") == 0) {
        for (int i = 1; i < frame_count; ++i) {
            if (frames[i].age < frames[victim].age) {
                victim = i;
            }
        }
    } else if (strcmp(algo, "opt") == 0) {
        int farthest = -1;
        for (int i = 0; i < frame_count; ++i) {
            int next_use = INT_MAX;
            for (int j = pos + 1; j < ref_count; ++j) {
                if (refs[j] == frames[i].page) {
                    next_use = j;
                    break;
                }
            }
            if (next_use > farthest) {
                farthest = next_use;
                victim = i;
            }
        }
    } else if (strcmp(algo, "clock") == 0) {
        while (1) {
            int h = *clock_hand % frame_count;
            if (frames[h].ref_bit == 0) {
                victim = h;
                *clock_hand = (h + 1) % frame_count;
                break;
            }
            frames[h].ref_bit = 0;
            *clock_hand = (h + 1) % frame_count;
        }
    } else {
        die("unknown page replacement algorithm; use fifo, lru, opt, or clock");
    }
    return victim;
}

static void print_frames(Frame frames[], int frame_count) {
    printf("frames=");
    for (int i = 0; i < frame_count; ++i) {
        if (frames[i].page < 0) {
            printf("[_]");
        } else {
            printf("[%d]", frames[i].page);
        }
    }
}

static void run_paging(const char *algo, int frame_count, const int refs[], int ref_count) {
    if (frame_count <= 0 || frame_count > MAX_FRAMES) {
        die("frames must be in 1..128");
    }
    Frame frames[MAX_FRAMES];
    int faults = 0;
    int hits = 0;
    int clock_hand = 0;
    for (int i = 0; i < frame_count; ++i) {
        frames[i] = (Frame){-1, -1, -1, 0};
    }
    printf("PAGING algo=%s frames=%d refs=%d\n", algo, frame_count, ref_count);
    for (int pos = 0; pos < ref_count; ++pos) {
        int page = refs[pos];
        int hit = find_page(frames, frame_count, page);
        if (hit >= 0) {
            hits++;
            frames[hit].age = pos;
            frames[hit].ref_bit = 1;
            printf("REF %d page=%d HIT ", pos + 1, page);
            print_frames(frames, frame_count);
            printf("\n");
            continue;
        }

        faults++;
        int target = empty_frame(frames, frame_count);
        int evicted = -1;
        if (target < 0) {
            target = choose_victim(algo, frames, frame_count, refs, ref_count, pos, &clock_hand);
            evicted = frames[target].page;
        }
        frames[target].page = page;
        frames[target].age = pos;
        frames[target].loaded_at = pos;
        frames[target].ref_bit = 1;
        printf("REF %d page=%d FAULT", pos + 1, page);
        if (evicted >= 0) {
            printf(" evict=%d", evicted);
        }
        printf(" ");
        print_frames(frames, frame_count);
        printf("\n");
    }
    double rate = ref_count > 0 ? (double)faults / ref_count : 0.0;
    printf("SUMMARY refs=%d frames=%d faults=%d hits=%d fault_rate=%.3f\n", ref_count, frame_count, faults, hits, rate);
    printf("ANALYSIS FIFO实现简单但可能出现Belady异常；LRU利用局部性；OPT是理论最优基准；Clock以较低成本近似LRU。\n");
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  %s partition --algo ff|bf|wf --input trace.txt [--size N]\n"
            "  %s paging --algo fifo|lru|opt|clock --frames N --refs \"7 0 1 ...\"\n"
            "  %s paging --algo fifo|lru|opt|clock --input refs.txt\n",
            argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "partition") == 0) {
        const char *algo = "ff";
        const char *input = NULL;
        int size = 0;
        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--algo") == 0 && i + 1 < argc) {
                algo = argv[++i];
            } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
                input = argv[++i];
            } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
                size = atoi(argv[++i]);
            } else {
                usage(argv[0]);
                return 1;
            }
        }
        if (!input) {
            usage(argv[0]);
            return 1;
        }
        run_partition(algo, input, size);
    } else if (strcmp(argv[1], "paging") == 0) {
        const char *algo = "fifo";
        const char *input = NULL;
        const char *refs_arg = NULL;
        int frames = 0;
        int refs[MAX_REFS];
        int ref_count = 0;
        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--algo") == 0 && i + 1 < argc) {
                algo = argv[++i];
            } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
                input = argv[++i];
            } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
                frames = atoi(argv[++i]);
            } else if (strcmp(argv[i], "--refs") == 0 && i + 1 < argc) {
                refs_arg = argv[++i];
            } else {
                usage(argv[0]);
                return 1;
            }
        }
        if (input) {
            parse_refs_from_file(input, &frames, refs, &ref_count);
        } else if (refs_arg) {
            ref_count = parse_refs_string(refs_arg, refs);
        } else {
            usage(argv[0]);
            return 1;
        }
        if (frames <= 0 || ref_count <= 0) {
            die("paging requires frames and references");
        }
        run_paging(algo, frames, refs, ref_count);
    } else {
        usage(argv[0]);
        return 1;
    }
    return 0;
}
