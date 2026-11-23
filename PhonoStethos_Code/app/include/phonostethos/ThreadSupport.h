#pragma once

#include <cerrno>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>

namespace phono_app {
// pthread functions return the error number themselves (not through errno).
inline void checkPthread(int error) {
    if (error != 0) {
        // Startup cannot continue with an uninitialized mutex or condition variable.
        std::fprintf(stderr, "pthread: %s\n", std::strerror(error));
        std::exit(EXIT_FAILURE);
    }
}

inline void initializeMutex(pthread_mutex_t& mutex) {
    checkPthread(pthread_mutex_init(&mutex, nullptr));
}

inline void initializeCondition(pthread_cond_t& condition) {
    pthread_condattr_t attributes;
    checkPthread(pthread_condattr_init(&attributes));
    const int clock_error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    int init_error = clock_error;
    if (clock_error == 0) {
        init_error = pthread_cond_init(&condition, &attributes);
    }
    pthread_condattr_destroy(&attributes);
    checkPthread(init_error);
}

// pthread_cond_timedwait needs an absolute deadline, not a duration.
inline timespec waitDeadline(long milliseconds) {
    timespec deadline{};
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

// This small scope guard only pairs pthread_mutex_lock with unlock.
// Returning early from a function still releases the mutex.
class PthreadLock {
public:
    explicit PthreadLock(pthread_mutex_t& mutex) : mutex_(mutex) {
        /* API: pthread_mutex_lock
         * Lock the mutex before reading or changing the associated shared data. Pair it with unlock after the protected work. Pthread errors are returned directly.
         */
        checkPthread(pthread_mutex_lock(&mutex_));
    }
    /* API: pthread_mutex_unlock
     * Release the mutex so another thread can use the shared object. The caller must own the mutex.
     */
    ~PthreadLock() { pthread_mutex_unlock(&mutex_); }
    PthreadLock(const PthreadLock&) = delete;
    PthreadLock& operator=(const PthreadLock&) = delete;
private:
    pthread_mutex_t& mutex_;
};
}  // namespace phono_app
