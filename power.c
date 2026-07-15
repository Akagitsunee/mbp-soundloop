#include <pthread.h>
#include <time.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/IOMessage.h>
#include <CoreFoundation/CoreFoundation.h>

extern volatile int g_quit; /* defined in main.c */

static pthread_mutex_t mu       = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv       = PTHREAD_COND_INITIALIZER;
static int             sleeping = 0;

static io_connect_t g_root;
static io_object_t  g_notifier;

/*
 * Do not touch PortAudio from this callback — Pa_StopStream blocks
 * waiting for CoreAudio to drain, but CoreAudio is already suspending,
 * causing a deadlock that prevents the wakeup notification from ever
 * arriving.  The main thread detects stalled ticks and closes the
 * stream itself after returning from route().
 */
static void power_cb(void *ref, io_service_t svc, natural_t msg, void *arg) {
    (void)ref; (void)svc;
    if (msg == kIOMessageSystemWillSleep) {
        IOAllowPowerChange(g_root, (long)arg);
        pthread_mutex_lock(&mu);
        sleeping = 1;
        pthread_mutex_unlock(&mu);
    } else if (msg == kIOMessageSystemHasPoweredOn) {
        pthread_mutex_lock(&mu);
        sleeping = 0;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
    }
}

static void *power_thread(void *arg) {
    (void)arg;
    IONotificationPortRef port;
    g_root = IORegisterForSystemPower(NULL, &port, power_cb, &g_notifier);
    CFRunLoopAddSource(CFRunLoopGetCurrent(),
                       IONotificationPortGetRunLoopSource(port),
                       kCFRunLoopDefaultMode);
    CFRunLoopRun();
    return NULL;
}

void init_power_notifications(void) {
    pthread_t t;
    pthread_create(&t, NULL, power_thread, NULL);
    pthread_detach(t);
}

/*
 * Block while the system is sleeping.  Uses a timed wait so that
 * Ctrl-C (which sets g_quit but cannot broadcast the cv from a signal
 * handler) is noticed within ~200 ms instead of hanging forever.
 */
void wait_if_sleeping(void) {
    pthread_mutex_lock(&mu);
    while (sleeping && !g_quit) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200000000L; /* 200 ms */
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&cv, &mu, &ts);
    }
    pthread_mutex_unlock(&mu);
}
