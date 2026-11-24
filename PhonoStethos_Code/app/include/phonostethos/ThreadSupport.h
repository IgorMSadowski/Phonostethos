/**
 * ThreadSupport.h - provides the small pthread helpers shared by queues, buffers and managers.
 *
 * CONNECTIONS: This header defines checked mutex/condition initialization, monotonic deadlines and PthreadLock. PthreadLock pairs lock/unlock through scope lifetime; it is not a queue or mailbox. Manager .cpp files still call the real pthread creation/scheduling APIs.
 *
 * FUNCTIONS:
 * - checkPthread(): handle pthread error numbers directly instead of reading errno.
 * - initializeMutex(): initialize one pthread mutex and reject initialization failure.
 * - initializeCondition(): initialize a condition variable that uses CLOCK_MONOTONIC deadlines.
 * - waitDeadline(): convert a relative millisecond delay into an absolute monotonic timespec.
 * - PthreadLock(): Pthread Lock.
 */

// API DOCUMENTATION FOR THIS FILE:
// POSIX threads: creation, joining, mutexes and condition variables:
// https://man7.org/linux/man-pages/man7/pthreads.7.html
// pthread mutex lock, try-lock and unlock:
// https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html
// Condition waits: mutex release/reacquisition and absolute timeouts:
// https://man7.org/linux/man-pages/man3/pthread_cond_timedwait.3p.html
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


#pragma once // Compile these declarations once per translation unit, even if included repeatedly.

#include <cerrno> // Include errno and named system-call error codes.
#include <pthread.h> // Include POSIX thread creation, mutexes and condition-variable APIs.
#include <cstdio> // Include C-style file/console formatting used by this file.
#include <cstdlib> // Include process exit, allocation or numeric conversion functions.
#include <cstring> // Include byte copying/comparison and readable system-error strings.
#include <time.h> // Include timespec, monotonic clocks and timed sleeps.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.
// pthread functions return the error number themselves (not through errno).
inline void checkPthread(int error) { // Handle pthread error numbers directly instead of reading errno.
    if (error != 0) { // Test the condition error != 0.
        // Startup cannot continue with an uninitialized mutex or condition variable.
        std::fprintf(stderr, "pthread: %s\n", std::strerror(error)); // Print the diagnostic or test result for the operator.
        std::exit(EXIT_FAILURE); // Complete the call above with these remaining arguments.
    } // End this block, declaration or argument list.
} // End this block, declaration or argument list.

inline void initializeMutex(pthread_mutex_t& mutex) { // Initialize one pthread mutex and reject initialization failure.
    /* API: pthread_mutex_init
     * Documentation: https://man7.org/linux/man-pages/man7/pthreads.7.html
     * Initialize the mutex at the supplied address; nullptr attributes select defaults. Returns 0 or an error number directly. Initialize once before sharing it.
     */
    checkPthread(pthread_mutex_init(&mutex, nullptr)); // Handle pthread error numbers directly instead of reading errno.
} // End this block, declaration or argument list.

inline void initializeCondition(pthread_cond_t& condition) { // Initialize a condition variable that uses CLOCK_MONOTONIC deadlines.
    pthread_condattr_t attributes; // Declare attributes for this operation.
    /* API: pthread_condattr_init
     * Documentation: https://man7.org/linux/man-pages/man7/pthreads.7.html
     * Initialize attributes used to create a condition variable. Check its returned pthread error number before using the attributes.
     */
    checkPthread(pthread_condattr_init(&attributes)); // Handle pthread error numbers directly instead of reading errno.
    /* API: pthread_condattr_setclock
     * Documentation: https://man7.org/linux/man-pages/man7/pthreads.7.html
     * Select CLOCK_MONOTONIC for condition deadlines. This keeps elapsed-time waits independent of wall-clock/calendar adjustments.
     */
    const int clock_error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC); // Initialize clock error to pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC).
    int init_error = clock_error; // Initialize init error to clock_error.
    if (clock_error == 0) { // Test the condition clock_error == 0.
        /* API: pthread_cond_init
         * Documentation: https://man7.org/linux/man-pages/man7/pthreads.7.html
         * Initialize the condition variable with its configured attributes. It wakes waiters; protected fields/queue records hold the actual state/data.
         */
        init_error = pthread_cond_init(&condition, &attributes); // Set init error to pthread_cond_init(&condition, &attributes).
    } // End this block, declaration or argument list.
    /* API: pthread_condattr_destroy
     * Documentation: https://man7.org/linux/man-pages/man7/pthreads.7.html
     * Release the temporary condition attributes after condition creation; it does not destroy the initialized condition variable.
     */
    pthread_condattr_destroy(&attributes); // Call pthread_condattr_destroy with the arguments shown here.
    checkPthread(init_error); // Handle pthread error numbers directly instead of reading errno.
} // End this block, declaration or argument list.

// pthread_cond_timedwait needs an absolute deadline, not a duration.
inline timespec waitDeadline(long milliseconds) { // Convert a relative millisecond delay into an absolute monotonic timespec.
    timespec deadline{}; // Declare deadline for this operation.
    /* API: clock_gettime
     * Documentation: https://man7.org/linux/man-pages/man3/clock_gettime.3.html
     * Read the selected clock into a timespec. CLOCK_MONOTONIC measures elapsed time; seconds/nanoseconds are used to form absolute wait deadlines.
     */
    clock_gettime(CLOCK_MONOTONIC, &deadline); // Call clock_gettime with the arguments shown here.
    deadline.tv_sec += milliseconds / 1000; // Update deadline.tv sec using += milliseconds / 1000.
    deadline.tv_nsec += (milliseconds % 1000) * 1000000L; // Update deadline.tv nsec using += (milliseconds % 1000) * 1000000L.
    if (deadline.tv_nsec >= 1000000000L) { // Test the condition deadline.tv_nsec >= 1000000000L.
        ++deadline.tv_sec; // Advance this index/counter for the next item.
        deadline.tv_nsec -= 1000000000L; // Update deadline.tv nsec using -= 1000000000L.
    } // End this block, declaration or argument list.
    return deadline; // Return deadline to the caller.
} // End this block, declaration or argument list.

// This small scope guard only pairs pthread_mutex_lock with unlock.
// Returning early from a function still releases the mutex.
// READING THE COMMUNICATION CODE: { PthreadLock lock(mutex); ... } locks on entry
// and unlocks at the closing brace. It protects the mailbox/queue/buffer data
// accessed in that scope; it does not itself store or transfer any message.
class PthreadLock { // Declare PthreadLock and its related operations/state.
public: // The class operations callable by other code.
    explicit PthreadLock(pthread_mutex_t& mutex) : mutex_(mutex) { // Finish the parameters/condition and enter the block below.
        /* API: pthread_mutex_lock
         * Documentation: https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html
         * Lock the mutex before reading or changing the associated shared data. Pair it with unlock after the protected work. Pthread errors are returned directly.
         */
        checkPthread(pthread_mutex_lock(&mutex_)); // Acquire the mutex before accessing the associated shared state.
    } // End this block, declaration or argument list.
    /* API: pthread_mutex_unlock
     * Documentation: https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html
     * Release the mutex so another thread can use the shared object. The caller must own the mutex.
     */
    ~PthreadLock() { pthread_mutex_unlock(&mutex_); } // Release the mutex owned by this thread before leaving the critical section.
    PthreadLock(const PthreadLock&) = delete; // Disallow copying this resource-owning object.
    PthreadLock& operator=(const PthreadLock&) = delete; // Disallow copying this resource-owning object.
private: // Internal helpers and state used by this class.
    pthread_mutex_t& mutex_; // Declare mutex for this class instance.
}; // End this block, declaration or argument list.
}  // namespace phono_app
