#include <stdbool.h>
// Request the project's exact thread settings on four short-lived test workers.
// This does not inspect or change the running PhonoStethos server's threads.
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char* name;
    int core;
    int policy;
    int priority;
    bool success;
} Settings;

static void* check_settings(void* argument) {
    Settings* wanted = (Settings*)(argument);
    wanted->success = true;
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(wanted->core, &cpus);
    // API: pthread_setaffinity_np restricts this thread to the selected CPU core.
    int result = pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    if (result != 0) {
        fprintf(stderr, "%s affinity: %s\n", wanted->name, strerror(result));
        wanted->success = false;
    }
    struct sched_param parameters = {0};
    parameters.sched_priority = wanted->priority;
    // API: pthread_setschedparam requests a policy and its static RT priority.
    result = pthread_setschedparam(pthread_self(), wanted->policy, &parameters);
    if (result != 0) {
        fprintf(stderr, "%s scheduling: %s\n", wanted->name, strerror(result));
        wanted->success = false;
    }
    if (wanted->policy == SCHED_OTHER) {
        // Linux nice is per thread. gettid identifies this worker, not all workers.
        if (setpriority(PRIO_PROCESS, syscall(SYS_gettid), 0) != 0) {
            perror("nice 0");
            wanted->success = false;
        }
    }
    int policy = -1;
    struct sched_param actual = {0};
    if (pthread_getschedparam(pthread_self(), &policy, &actual) != 0) wanted->success = false;
    cpu_set_t actual_cpus;
    CPU_ZERO(&actual_cpus);
    if (pthread_getaffinity_np(pthread_self(), sizeof(actual_cpus), &actual_cpus) != 0) wanted->success = false;
    if (!CPU_ISSET(wanted->core, &actual_cpus) || CPU_COUNT(&actual_cpus) != 1 ||
        policy != wanted->policy || actual.sched_priority != wanted->priority) wanted->success = false;
    errno = 0;
    int nice_value = getpriority(PRIO_PROCESS, syscall(SYS_gettid));
    if (errno != 0 || (wanted->policy == SCHED_OTHER && nice_value != 0)) wanted->success = false;
    const char* policy_name = "OTHER/UNEXPECTED";
    if (policy == SCHED_OTHER) policy_name = "SCHED_OTHER";
    if (policy == SCHED_FIFO) policy_name = "SCHED_FIFO";
    printf("%s: requested core %d; actual CPU %d, %s priority %d, nice %d -> %s\n",
                wanted->name, wanted->core, sched_getcpu(), policy_name,
                actual.sched_priority, nice_value, wanted->success ? "PASS" : "FAIL");
    return NULL; // A FIFO test worker performs no endless loop or audio processing.
}

int main(void) {
    Settings tests[4] = {
        {"th_Main", 0, SCHED_OTHER, 0, false},
        {"th_Network", 3, SCHED_OTHER, 0, false},
        {"th_AudioDSP", 1, SCHED_FIFO, 80, false},
        {"th_FileIO", 2, SCHED_FIFO, 50, false}
    };
    bool success = true;
    for (int i = 0; i < 4; i++) {
        pthread_t worker;
        int result = pthread_create(&worker, NULL, check_settings, &tests[i]);
        if (result != 0) {
            fprintf(stderr, "FAIL: create worker: %s\n", strerror(result));
            return 1;
        }
        if (pthread_join(worker, NULL) != 0 || !tests[i].success) success = false;
    }
    if (!success) {
        puts("FAIL: check permissions (root/CAP_SYS_NICE), CPU availability and RT limits on the Pi.");
        return 1;
    }
    puts("PASS: all four scheduling requests/readbacks match the design");
    return 0;
}
