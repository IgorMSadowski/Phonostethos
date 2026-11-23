/**
 * SchedulerTest.c - checks Scheduler Test with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - check_settings(): check settings.
 * - main(): run this standalone test and return success only if its checks pass.
 */

#include <stdbool.h> // Include C bool, true and false.
// Request the project's exact thread settings on four short-lived test workers.
// This does not inspect or change the running PhonoStethos server's threads.
#include <pthread.h> // Include POSIX thread creation, mutexes and condition-variable APIs.
#include <sched.h> // Include CPU affinity masks and Linux scheduling policies/priorities.
#include <sys/resource.h> // Include Linux nice-value control, distinct from real-time scheduler priority.
#include <sys/syscall.h> // Include the sys/syscall.h declarations used by this source file.
#include <unistd.h> // Include POSIX descriptor read/write/close and synchronization calls.
#include <errno.h> // Include errno and symbolic system-call error codes.
#include <stdio.h> // Include printf/fprintf/puts console diagnostics.
#include <string.h> // Include memory/string copying, comparisons and strerror diagnostics.

typedef struct { // Declare struct for this operation.
    const char* name; // Declare name for this operation.
    int core; // Declare core for this operation.
    int policy; // Declare policy for this operation.
    int priority; // Declare priority for this operation.
    bool success; // Declare success for this operation.
} Settings; // Complete this expression or declaration.

static void* check_settings(void* argument) { // Finish the parameters/condition and enter the block below.
    Settings* wanted = (Settings*)(argument); // Initialize wanted to (Settings*)(argument).
    wanted->success = true; // Set wanted->success to true.
    cpu_set_t cpus; // Declare cpus for this operation.
    CPU_ZERO(&cpus); // Call CPU_ZERO with the arguments shown here.
    CPU_SET(wanted->core, &cpus); // Call CPU_SET with the arguments shown here.
    // API: pthread_setaffinity_np restricts this thread to the selected CPU core.
    /* API: pthread_self
     * Return the calling pthread handle, used when setting this worker's affinity or scheduling policy.
     */
    int result = pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus); // Initialize result to pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus).
    if (result != 0) { // Test the condition result != 0.
        fprintf(stderr, "%s affinity: %s\n", wanted->name, strerror(result)); // Print the diagnostic or test result for the operator.
        wanted->success = false; // Set wanted->success to false.
    } // End this block, declaration or argument list.
    struct sched_param parameters = {0}; // Group the related fields in one plain data record.
    parameters.sched_priority = wanted->priority; // Set parameters.sched priority to wanted->priority.
    // API: pthread_setschedparam requests a policy and its static RT priority.
    result = pthread_setschedparam(pthread_self(), wanted->policy, &parameters); // Set result to pthread_setschedparam(pthread_self(), wanted->policy, &parameters).
    if (result != 0) { // Test the condition result != 0.
        fprintf(stderr, "%s scheduling: %s\n", wanted->name, strerror(result)); // Print the diagnostic or test result for the operator.
        wanted->success = false; // Set wanted->success to false.
    } // End this block, declaration or argument list.
    if (wanted->policy == SCHED_OTHER) { // Test the condition wanted->policy == SCHED_OTHER.
        // Linux nice is per thread. gettid identifies this worker, not all workers.
        /* API: setpriority
         * Set the calling Linux thread's nice value with PRIO_PROCESS and ID 0. Nice 0 is normal SCHED_OTHER weight; it is distinct from FIFO priority 50/80. Returns 0 or -1 with errno.
         */
        if (setpriority(PRIO_PROCESS, syscall(SYS_gettid), 0) != 0) { // Test the condition setpriority(PRIO_PROCESS, syscall(SYS_gettid), 0) != 0.
            perror("nice 0"); // Call perror with the arguments shown here.
            wanted->success = false; // Set wanted->success to false.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    int policy = -1; // Initialize policy to -1.
    struct sched_param actual = {0}; // Group the related fields in one plain data record.
    if (pthread_getschedparam(pthread_self(), &policy, &actual) != 0) wanted->success = false; // Test the condition pthread_getschedparam(pthread_self(), &policy, &actual) != 0. If true, set wanted->success to false.
    cpu_set_t actual_cpus; // Declare actual cpus for this operation.
    CPU_ZERO(&actual_cpus); // Call CPU_ZERO with the arguments shown here.
    if (pthread_getaffinity_np(pthread_self(), sizeof(actual_cpus), &actual_cpus) != 0) wanted->success = false; // Test the condition pthread_getaffinity_np(pthread_self(), sizeof(actual_cpus), &actual_cpus) != 0. If true, set wanted->success to false.
    if (!CPU_ISSET(wanted->core, &actual_cpus) || CPU_COUNT(&actual_cpus) != 1 || // Test the condition !CPU_ISSET(wanted->core, &actual_cpus) || CPU_COUNT(&actual_cpus) != 1 ||.
        policy != wanted->policy || actual.sched_priority != wanted->priority) wanted->success = false; // Complete this expression or declaration.
    errno = 0; // Set errno to 0.
    int nice_value = getpriority(PRIO_PROCESS, syscall(SYS_gettid)); // Initialize nice value to getpriority(PRIO_PROCESS, syscall(SYS_gettid)).
    if (errno != 0 || (wanted->policy == SCHED_OTHER && nice_value != 0)) wanted->success = false; // Test the condition errno != 0 || (wanted->policy == SCHED_OTHER && nice_value != 0). If true, set wanted->success to false.
    const char* policy_name = "OTHER/UNEXPECTED"; // Initialize policy name to "OTHER/UNEXPECTED".
    if (policy == SCHED_OTHER) policy_name = "SCHED_OTHER"; // Test the condition policy == SCHED_OTHER. If true, set policy name to "sched_other".
    if (policy == SCHED_FIFO) policy_name = "SCHED_FIFO"; // Test the condition policy == SCHED_FIFO. If true, set policy name to "sched_fifo".
    printf("%s: requested core %d; actual CPU %d, %s priority %d, nice %d -> %s\n", // Print the diagnostic or test result for the operator.
                wanted->name, wanted->core, sched_getcpu(), policy_name, // Pass this next argument/value to the call or initializer above.
                actual.sched_priority, nice_value, wanted->success ? "PASS" : "FAIL"); // Complete the call above with these remaining arguments.
    return NULL; // A FIFO test worker performs no endless loop or audio processing.
} // End this block, declaration or argument list.

int main(void) { // Run this standalone test and return success only if its checks pass.
    Settings tests[4] = { // Initialize tests to {.
        {"th_Main", 0, SCHED_OTHER, 0, false}, // Pass this next argument/value to the call or initializer above.
        {"th_Network", 3, SCHED_OTHER, 0, false}, // Pass this next argument/value to the call or initializer above.
        {"th_AudioDSP", 1, SCHED_FIFO, 80, false}, // Pass this next argument/value to the call or initializer above.
        {"th_FileIO", 2, SCHED_FIFO, 50, false} // Continue the expression or parameter list from the preceding line.
    }; // End this block, declaration or argument list.
    bool success = true; // Initialize success to true.
    for (int i = 0; i < 4; i++) { // Visit the specified items/indexes in order.
        pthread_t worker; // Declare worker for this operation.
        int result = pthread_create(&worker, NULL, check_settings, &tests[i]); // Initialize result to pthread_create(&worker, NULL, check_settings, &tests[i]).
        if (result != 0) { // Test the condition result != 0.
            fprintf(stderr, "FAIL: create worker: %s\n", strerror(result)); // Print the diagnostic or test result for the operator.
            return 1; // Return 1 to the caller.
        } // End this block, declaration or argument list.
        if (pthread_join(worker, NULL) != 0 || !tests[i].success) success = false; // Test the condition pthread_join(worker, NULL) != 0 || !tests[i].success. If true, set success to false.
    } // End this block, declaration or argument list.
    if (!success) { // Test the condition !success.
        puts("FAIL: check permissions (root/CAP_SYS_NICE), CPU availability and RT limits on the Pi."); // Call puts with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    puts("PASS: all four scheduling requests/readbacks match the design"); // Call puts with the arguments shown here.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.
