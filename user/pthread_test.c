#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

static int shared_counter;
static pthread_mutex_t mutex;

static void *thread_func(void *arg) {
    int id = (int)(long)arg;
    printf("thread %d: started (tid=%d, pid=%d)\n", id, gettid(), getpid());

    for (int i = 0; i < 5; i++) {
        pthread_mutex_lock(&mutex);
        shared_counter++;
        printf("thread %d: counter=%d\n", id, shared_counter);
        pthread_mutex_unlock(&mutex);
        sched_yield();
    }

    printf("thread %d: done\n", id);
    return (void *)(long)(id * 100);
}

int main(void) {
    printf("pthread test: main tid=%d pid=%d\n", gettid(), getpid());

    pthread_mutex_init(&mutex, NULL);

    pthread_t t1, t2;
    int r;

    r = pthread_create(&t1, NULL, thread_func, (void *)1);
    if (r != 0) {
        printf("pthread_create t1 failed: %d\n", r);
        return 1;
    }
    printf("created thread 1: tid=%lu\n", (unsigned long)t1);

    r = pthread_create(&t2, NULL, thread_func, (void *)2);
    if (r != 0) {
        printf("pthread_create t2 failed: %d\n", r);
        return 1;
    }
    printf("created thread 2: tid=%lu\n", (unsigned long)t2);

    void *ret1, *ret2;
    pthread_join(t1, &ret1);
    pthread_join(t2, &ret2);

    printf("thread 1 joined, ret=%ld\n", (long)ret1);
    printf("thread 2 joined, ret=%ld\n", (long)ret2);
    printf("final counter=%d\n", shared_counter);
    printf("pthread test: PASS\n");

    return 0;
}
