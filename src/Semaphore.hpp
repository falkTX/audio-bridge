// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#if defined(__APPLE__)
#include <mach/mach_types.h>
#include <mach/semaphore.h>
typedef struct {
    task_t task;
    semaphore_t sem;
} d_semaphore;
extern "C" {
task_t mach_task_self();
kern_return_t semaphore_create(task_t, semaphore_t*, int, int);
kern_return_t semaphore_destroy(task_t, semaphore_t);
}
#else
#include <semaphore.h>
typedef sem_t d_semaphore;
#endif

namespace x {

static inline
bool semaphore_init(d_semaphore* const sem, const unsigned int value = 0)
{
   #if defined(__APPLE__)
    sem->task = mach_task_self();
    return ::semaphore_create(sem->task, &sem->sem, SYNC_POLICY_FIFO, value) == KERN_SUCCESS;
   #else
    return sem_init(sem, 0, value) == 0;
   #endif
}

static inline
bool semaphore_destroy(d_semaphore* const sem)
{
   #if defined(__APPLE__)
    return ::semaphore_destroy(sem->task, sem->sem) == KERN_SUCCESS;
   #else
    return sem_destroy(sem) == 0;
   #endif
}

static inline
bool semaphore_post(d_semaphore* const sem)
{
   #if defined(__APPLE__)
    return ::semaphore_signal(sem->sem) == KERN_SUCCESS;
   #else
    return sem_post(sem) == 0;
   #endif
}

static inline
bool semaphore_timedwait(d_semaphore* const sem, const unsigned int nsec)
{
   #if defined(__APPLE__)
    const mach_timespec_t time = { 0, static_cast<clock_res_t>(nsec) };
    return ::semaphore_timedwait(sem->sem, time) == KERN_SUCCESS;
   #else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    ts.tv_nsec += nsec;
    if (ts.tv_nsec >= 1000000000LL)
    {
        ++ts.tv_sec;
        ts.tv_nsec -= 1000000000LL;
    }

    for (int r;;)
    {
        r = sem_timedwait(sem, &ts);

        if (r < 0)
            r = errno;

        if (r == EINTR)
            continue;

        return r == 0;
    }
   #endif
}

static inline
bool semaphore_trywait(d_semaphore* const sem)
{
   #if defined(__APPLE__)
    const mach_timespec_t nonblocking = { 0, 0 };
    return ::semaphore_timedwait(sem->sem, nonblocking) == KERN_SUCCESS;
   #else
    return sem_trywait(sem) == 0;
   #endif
}

static inline
bool semaphore_wait(d_semaphore* const sem)
{
   #if defined(__APPLE__)
    return ::semaphore_wait(sem->sem) == KERN_SUCCESS;
   #else
    return sem_wait(sem) == 0;
   #endif
}

}

using namespace x;
