#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

static void die(const char *msg) {
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

static int opt_int(int argc, char **argv, const char *name, int fallback) {
    for (int i = 2; i + 1 < argc; ++i) {
        if (strcmp(argv[i], name) == 0) {
            return atoi(argv[i + 1]);
        }
    }
    return fallback;
}

static void log_event(const char *fmt, int a, int b, int c) {
    pthread_mutex_lock(&print_lock);
    printf(fmt, a, b, c);
    pthread_mutex_unlock(&print_lock);
}

static void sleep_us(long usec) {
    struct timespec ts;
    ts.tv_sec = usec / 1000000L;
    ts.tv_nsec = (usec % 1000000L) * 1000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

typedef struct {
    int *buffer;
    int capacity;
    int head;
    int tail;
    sem_t empty;
    sem_t full;
    pthread_mutex_t buffer_lock;
    pthread_mutex_t id_lock;
    pthread_mutex_t seen_lock;
    int total_items;
    int next_item;
    int produced;
    int consumed;
    int duplicates;
    int *seen;
} PcState;

static int pc_put(PcState *s, int item) {
    sem_wait(&s->empty);
    pthread_mutex_lock(&s->buffer_lock);
    s->buffer[s->tail] = item;
    s->tail = (s->tail + 1) % s->capacity;
    int tail_snapshot = s->tail;
    pthread_mutex_unlock(&s->buffer_lock);
    sem_post(&s->full);
    return tail_snapshot;
}

static int pc_get(PcState *s, int *head_snapshot) {
    sem_wait(&s->full);
    pthread_mutex_lock(&s->buffer_lock);
    int item = s->buffer[s->head];
    s->head = (s->head + 1) % s->capacity;
    *head_snapshot = s->head;
    pthread_mutex_unlock(&s->buffer_lock);
    sem_post(&s->empty);
    return item;
}

typedef struct {
    PcState *state;
    int id;
} ThreadArg;

static void *producer_main(void *arg) {
    ThreadArg *ta = arg;
    PcState *s = ta->state;
    while (1) {
        pthread_mutex_lock(&s->id_lock);
        if (s->next_item > s->total_items) {
            pthread_mutex_unlock(&s->id_lock);
            break;
        }
        int item = s->next_item++;
        s->produced++;
        pthread_mutex_unlock(&s->id_lock);

        int tail_snapshot = pc_put(s, item);
        log_event("producer=%d produced item=%d buffer_tail=%d\n", ta->id, item, tail_snapshot);
        sleep_us(1000);
    }
    return NULL;
}

static void *consumer_main(void *arg) {
    ThreadArg *ta = arg;
    PcState *s = ta->state;
    while (1) {
        int head_snapshot = 0;
        int item = pc_get(s, &head_snapshot);
        if (item < 0) {
            log_event("consumer=%d received_stop item=%d buffer_head=%d\n", ta->id, item, head_snapshot);
            break;
        }
        pthread_mutex_lock(&s->seen_lock);
        if (s->seen[item]) {
            s->duplicates++;
        } else {
            s->seen[item] = 1;
        }
        s->consumed++;
        pthread_mutex_unlock(&s->seen_lock);
        log_event("consumer=%d consumed item=%d buffer_head=%d\n", ta->id, item, head_snapshot);
        sleep_us(1500);
    }
    return NULL;
}

static int run_producer_consumer(int producers, int consumers, int items, int capacity) {
    if (producers <= 0 || consumers <= 0 || items <= 0 || capacity <= 0) {
        die("producer_consumer options must be positive");
    }
    PcState s;
    memset(&s, 0, sizeof(s));
    s.capacity = capacity;
    s.total_items = items;
    s.next_item = 1;
    s.buffer = calloc((size_t)capacity, sizeof(*s.buffer));
    s.seen = calloc((size_t)items + 1, sizeof(*s.seen));
    if (!s.buffer || !s.seen) {
        die("calloc failed");
    }
    pthread_mutex_init(&s.buffer_lock, NULL);
    pthread_mutex_init(&s.id_lock, NULL);
    pthread_mutex_init(&s.seen_lock, NULL);
    sem_init(&s.empty, 0, (unsigned int)capacity);
    sem_init(&s.full, 0, 0);

    pthread_t *pt = calloc((size_t)producers, sizeof(*pt));
    pthread_t *ct = calloc((size_t)consumers, sizeof(*ct));
    ThreadArg *pa = calloc((size_t)producers, sizeof(*pa));
    ThreadArg *ca = calloc((size_t)consumers, sizeof(*ca));
    if (!pt || !ct || !pa || !ca) {
        die("calloc threads failed");
    }
    printf("PRODUCER_CONSUMER producers=%d consumers=%d items=%d buffer=%d\n", producers, consumers, items, capacity);
    for (int i = 0; i < producers; ++i) {
        pa[i] = (ThreadArg){&s, i};
        pthread_create(&pt[i], NULL, producer_main, &pa[i]);
    }
    for (int i = 0; i < consumers; ++i) {
        ca[i] = (ThreadArg){&s, i};
        pthread_create(&ct[i], NULL, consumer_main, &ca[i]);
    }
    for (int i = 0; i < producers; ++i) {
        pthread_join(pt[i], NULL);
    }
    for (int i = 0; i < consumers; ++i) {
        (void)pc_put(&s, -1);
    }
    for (int i = 0; i < consumers; ++i) {
        pthread_join(ct[i], NULL);
    }

    int missing = 0;
    for (int i = 1; i <= items; ++i) {
        if (!s.seen[i]) {
            missing++;
        }
    }
    int pass = s.produced == items && s.consumed == items && missing == 0 && s.duplicates == 0;
    printf("SUMMARY produced=%d consumed=%d duplicates=%d missing=%d result=%s\n",
           s.produced, s.consumed, s.duplicates, missing, pass ? "PASS" : "FAIL");

    sem_destroy(&s.empty);
    sem_destroy(&s.full);
    pthread_mutex_destroy(&s.buffer_lock);
    pthread_mutex_destroy(&s.id_lock);
    pthread_mutex_destroy(&s.seen_lock);
    free(s.buffer);
    free(s.seen);
    free(pt);
    free(ct);
    free(pa);
    free(ca);
    return pass ? 0 : 1;
}

typedef struct {
    sem_t turnstile;
    sem_t room_empty;
    pthread_mutex_t read_count_lock;
    int read_count;
    int value;
    int writes;
    int reads;
    int loops;
} RwState;

typedef struct {
    RwState *state;
    int id;
} RwArg;

static void *reader_main(void *arg) {
    RwArg *ra = arg;
    RwState *s = ra->state;
    for (int i = 0; i < s->loops; ++i) {
        sem_wait(&s->turnstile);
        sem_post(&s->turnstile);

        pthread_mutex_lock(&s->read_count_lock);
        s->read_count++;
        if (s->read_count == 1) {
            sem_wait(&s->room_empty);
        }
        pthread_mutex_unlock(&s->read_count_lock);

        int snapshot = s->value;
        __sync_fetch_and_add(&s->reads, 1);
        log_event("reader=%d loop=%d value=%d\n", ra->id, i + 1, snapshot);
        sleep_us(800);

        pthread_mutex_lock(&s->read_count_lock);
        s->read_count--;
        if (s->read_count == 0) {
            sem_post(&s->room_empty);
        }
        pthread_mutex_unlock(&s->read_count_lock);
    }
    return NULL;
}

static void *writer_main(void *arg) {
    RwArg *wa = arg;
    RwState *s = wa->state;
    for (int i = 0; i < s->loops; ++i) {
        sem_wait(&s->turnstile);
        sem_wait(&s->room_empty);

        int before = s->value;
        s->value = before + 1;
        s->writes++;
        log_event("writer=%d loop=%d value=%d\n", wa->id, i + 1, s->value);
        sleep_us(1000);

        sem_post(&s->room_empty);
        sem_post(&s->turnstile);
    }
    return NULL;
}

static int run_readers_writers(int readers, int writers, int loops) {
    if (readers <= 0 || writers <= 0 || loops <= 0) {
        die("readers_writers options must be positive");
    }
    RwState s;
    memset(&s, 0, sizeof(s));
    s.loops = loops;
    sem_init(&s.turnstile, 0, 1);
    sem_init(&s.room_empty, 0, 1);
    pthread_mutex_init(&s.read_count_lock, NULL);

    int total = readers + writers;
    pthread_t *threads = calloc((size_t)total, sizeof(*threads));
    RwArg *args = calloc((size_t)total, sizeof(*args));
    if (!threads || !args) {
        die("calloc failed");
    }
    printf("READERS_WRITERS readers=%d writers=%d loops=%d policy=fair_turnstile\n", readers, writers, loops);
    int k = 0;
    for (int i = 0; i < readers; ++i) {
        args[k] = (RwArg){&s, i};
        pthread_create(&threads[k], NULL, reader_main, &args[k]);
        k++;
    }
    for (int i = 0; i < writers; ++i) {
        args[k] = (RwArg){&s, i};
        pthread_create(&threads[k], NULL, writer_main, &args[k]);
        k++;
    }
    for (int i = 0; i < total; ++i) {
        pthread_join(threads[i], NULL);
    }
    int expected = writers * loops;
    int pass = s.value == expected && s.writes == expected && s.reads == readers * loops;
    printf("SUMMARY final_value=%d expected=%d reads=%d writes=%d result=%s\n",
           s.value, expected, s.reads, s.writes, pass ? "PASS" : "FAIL");

    sem_destroy(&s.turnstile);
    sem_destroy(&s.room_empty);
    pthread_mutex_destroy(&s.read_count_lock);
    free(threads);
    free(args);
    return pass ? 0 : 1;
}

typedef struct {
    int philosophers;
    int meals;
    pthread_mutex_t *forks;
    sem_t room;
    int *eaten;
} DiningState;

typedef struct {
    DiningState *state;
    int id;
} DiningArg;

static void *philosopher_main(void *arg) {
    DiningArg *pa = arg;
    DiningState *s = pa->state;
    int left = pa->id;
    int right = (pa->id + 1) % s->philosophers;
    int first = left < right ? left : right;
    int second = left < right ? right : left;

    for (int meal = 1; meal <= s->meals; ++meal) {
        sleep_us(500);
        sem_wait(&s->room);
        pthread_mutex_lock(&s->forks[first]);
        pthread_mutex_lock(&s->forks[second]);

        s->eaten[pa->id]++;
        log_event("philosopher=%d meal=%d forks=%d\n", pa->id, meal, first);
        sleep_us(700);

        pthread_mutex_unlock(&s->forks[second]);
        pthread_mutex_unlock(&s->forks[first]);
        sem_post(&s->room);
    }
    return NULL;
}

static int run_dining(int philosophers, int meals) {
    if (philosophers < 2 || meals <= 0) {
        die("dining_philosophers requires philosophers>=2 and meals>0");
    }
    DiningState s;
    s.philosophers = philosophers;
    s.meals = meals;
    s.forks = calloc((size_t)philosophers, sizeof(*s.forks));
    s.eaten = calloc((size_t)philosophers, sizeof(*s.eaten));
    if (!s.forks || !s.eaten) {
        die("calloc failed");
    }
    for (int i = 0; i < philosophers; ++i) {
        pthread_mutex_init(&s.forks[i], NULL);
    }
    sem_init(&s.room, 0, (unsigned int)(philosophers - 1));

    pthread_t *threads = calloc((size_t)philosophers, sizeof(*threads));
    DiningArg *args = calloc((size_t)philosophers, sizeof(*args));
    if (!threads || !args) {
        die("calloc failed");
    }
    printf("DINING_PHILOSOPHERS philosophers=%d meals=%d strategy=room_limit_ordered_forks\n", philosophers, meals);
    for (int i = 0; i < philosophers; ++i) {
        args[i] = (DiningArg){&s, i};
        pthread_create(&threads[i], NULL, philosopher_main, &args[i]);
    }
    for (int i = 0; i < philosophers; ++i) {
        pthread_join(threads[i], NULL);
    }
    int pass = 1;
    printf("EATEN");
    for (int i = 0; i < philosophers; ++i) {
        printf(" P%d=%d", i, s.eaten[i]);
        if (s.eaten[i] != meals) {
            pass = 0;
        }
    }
    printf("\nSUMMARY expected_each=%d result=%s\n", meals, pass ? "PASS" : "FAIL");

    for (int i = 0; i < philosophers; ++i) {
        pthread_mutex_destroy(&s.forks[i]);
    }
    sem_destroy(&s.room);
    free(s.forks);
    free(s.eaten);
    free(threads);
    free(args);
    return pass ? 0 : 1;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  %s producer_consumer --producers N --consumers N --items N --buffer N\n"
            "  %s readers_writers --readers N --writers N --loops N\n"
            "  %s dining_philosophers --philosophers N --meals N\n",
            argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "producer_consumer") == 0) {
        return run_producer_consumer(opt_int(argc, argv, "--producers", 2),
                                     opt_int(argc, argv, "--consumers", 2),
                                     opt_int(argc, argv, "--items", 12),
                                     opt_int(argc, argv, "--buffer", 4));
    }
    if (strcmp(argv[1], "readers_writers") == 0) {
        return run_readers_writers(opt_int(argc, argv, "--readers", 3),
                                   opt_int(argc, argv, "--writers", 2),
                                   opt_int(argc, argv, "--loops", 3));
    }
    if (strcmp(argv[1], "dining_philosophers") == 0) {
        return run_dining(opt_int(argc, argv, "--philosophers", 5),
                          opt_int(argc, argv, "--meals", 3));
    }
    usage(argv[0]);
    return 1;
}
