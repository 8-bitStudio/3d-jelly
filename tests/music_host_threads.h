/* pthread implementation of the libctru primitives used by the real workers. */
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
typedef int32_t s32;
typedef int64_t s64;
#define U64_MAX UINT64_MAX
#define CUR_THREAD_HANDLE 0
#define RESET_ONESHOT 0
typedef pthread_mutex_t LightLock;
typedef struct { pthread_mutex_t lock; pthread_cond_t cv; bool signaled; } LightEvent;
typedef struct HostThread { pthread_t id; void (*entry)(void *); void *arg; } *Thread;
static int host_thread_fail_after = -1;
static bool g_core1_reader_available = true;
static atomic_int host_thread_count;
static void LightLock_Init(LightLock *lock) { assert(!pthread_mutex_init(lock, NULL)); }
static void LightLock_Lock(LightLock *lock) { assert(!pthread_mutex_lock(lock)); }
static void LightLock_Unlock(LightLock *lock) { assert(!pthread_mutex_unlock(lock)); }
static void LightEvent_Init(LightEvent *event, int type)
{
    (void)type;
    pthread_mutex_init(&event->lock, NULL); pthread_cond_init(&event->cv, NULL); event->signaled = false;
}
static void LightEvent_Signal(LightEvent *event)
{
    pthread_mutex_lock(&event->lock); event->signaled = true;
    pthread_cond_signal(&event->cv); pthread_mutex_unlock(&event->lock);
}
static void LightEvent_Clear(LightEvent *event)
{
    pthread_mutex_lock(&event->lock); event->signaled = false; pthread_mutex_unlock(&event->lock);
}
static int LightEvent_WaitTimeout(LightEvent *event, s64 ns)
{
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += ns / 1000000000; deadline.tv_nsec += ns % 1000000000;
    if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&event->lock);
    int result = 0;
    while (!event->signaled && !result) result = pthread_cond_timedwait(&event->cv, &event->lock, &deadline);
    if (event->signaled) event->signaled = false;
    pthread_mutex_unlock(&event->lock);
    return result;
}
static void *host_thread_entry(void *arg)
{
    Thread thread = arg;
    atomic_fetch_add(&host_thread_count, 1);
    thread->entry(thread->arg);
    atomic_fetch_sub(&host_thread_count, 1);
    return NULL;
}
static Thread threadCreate(void (*entry)(void *), void *arg, size_t stack, int priority, int core, bool detached)
{
    (void)stack; (void)priority; (void)core; assert(!detached);
    if (host_thread_fail_after == 0) return NULL;
    if (host_thread_fail_after > 0) --host_thread_fail_after;
    Thread thread = malloc(sizeof(*thread)); assert(thread);
    thread->entry = entry; thread->arg = arg;
    assert(!pthread_create(&thread->id, NULL, host_thread_entry, thread));
    return thread;
}
static int threadJoin(Thread thread, u64 timeout) { (void)timeout; return pthread_join(thread->id, NULL); }
static void threadFree(Thread thread) { free(thread); }
static int svcGetThreadPriority(s32 *priority, int handle) { (void)handle; *priority = 0x30; return 0; }
static void svcSleepThread(s64 ns) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, NULL); }
