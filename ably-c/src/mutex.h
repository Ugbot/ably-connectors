/*
 * Copyright 2024 Ben Gamble
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ABLY_MUTEX_H
#define ABLY_MUTEX_H

#include <stdint.h>

/*
 * Portable mutex and condition variable wrappers.
 *
 *   POSIX (Linux, macOS, Android, iOS): pthread_mutex_t / pthread_cond_t
 *   Windows: CRITICAL_SECTION / CONDITION_VARIABLE
 *
 * Thread entry points use ABLY_THREAD_FUNC for the return-type difference.
 */

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

typedef CRITICAL_SECTION   ably_mutex_t;
typedef CONDITION_VARIABLE ably_cond_t;
typedef HANDLE             ably_thread_t;

#define ABLY_THREAD_FUNC DWORD WINAPI

static inline void ably_mutex_init(ably_mutex_t *m)
    { InitializeCriticalSection(m); }
static inline void ably_mutex_destroy(ably_mutex_t *m)
    { DeleteCriticalSection(m); }
static inline void ably_mutex_lock(ably_mutex_t *m)
    { EnterCriticalSection(m); }
static inline void ably_mutex_unlock(ably_mutex_t *m)
    { LeaveCriticalSection(m); }

static inline void ably_cond_init(ably_cond_t *c)
    { InitializeConditionVariable(c); }
static inline void ably_cond_destroy(ably_cond_t *c)
    { (void)c; }
static inline void ably_cond_signal(ably_cond_t *c)
    { WakeConditionVariable(c); }
static inline void ably_cond_broadcast(ably_cond_t *c)
    { WakeAllConditionVariable(c); }
static inline void ably_cond_wait(ably_cond_t *c, ably_mutex_t *m)
    { SleepConditionVariableCS(c, m, INFINITE); }
/* Returns 0 on success, non-zero on timeout. */
static inline int ably_cond_timedwait_ms(ably_cond_t *c, ably_mutex_t *m,
                                          int timeout_ms)
{
    return SleepConditionVariableCS(c, m, (DWORD)timeout_ms) ? 0 : 1;
}

int  ably_thread_create(ably_thread_t *t,
                         DWORD (WINAPI *fn)(LPVOID), void *arg);
void ably_thread_join(ably_thread_t t);

#else /* POSIX */

#  include <pthread.h>
#  include <errno.h>

typedef pthread_mutex_t ably_mutex_t;
typedef pthread_cond_t  ably_cond_t;
typedef pthread_t       ably_thread_t;

#define ABLY_THREAD_FUNC void *

static inline void ably_mutex_init(ably_mutex_t *m)
    { pthread_mutex_init(m, NULL); }
static inline void ably_mutex_destroy(ably_mutex_t *m)
    { pthread_mutex_destroy(m); }
static inline void ably_mutex_lock(ably_mutex_t *m)
    { pthread_mutex_lock(m); }
static inline void ably_mutex_unlock(ably_mutex_t *m)
    { pthread_mutex_unlock(m); }

static inline void ably_cond_init(ably_cond_t *c)
    { pthread_cond_init(c, NULL); }
static inline void ably_cond_destroy(ably_cond_t *c)
    { pthread_cond_destroy(c); }
static inline void ably_cond_signal(ably_cond_t *c)
    { pthread_cond_signal(c); }
static inline void ably_cond_broadcast(ably_cond_t *c)
    { pthread_cond_broadcast(c); }
static inline void ably_cond_wait(ably_cond_t *c, ably_mutex_t *m)
    { pthread_cond_wait(c, m); }

/* Returns 0 on success, non-zero on timeout. */
static inline int ably_cond_timedwait_ms(ably_cond_t *c, ably_mutex_t *m,
                                          int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    int rc = pthread_cond_timedwait(c, m, &ts);
    return (rc == ETIMEDOUT) ? 1 : 0;
}

int  ably_thread_create(ably_thread_t *t,
                         void *(*fn)(void *), void *arg);
void ably_thread_join(ably_thread_t t);

#endif /* _WIN32 */

#endif /* ABLY_MUTEX_H */
