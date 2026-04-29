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

#include "mutex.h"

#ifdef _WIN32

int ably_thread_create(ably_thread_t *t,
                        DWORD (WINAPI *fn)(LPVOID), void *arg)
{
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return (*t == NULL) ? -1 : 0;
}

void ably_thread_join(ably_thread_t t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

#else /* POSIX */

int ably_thread_create(ably_thread_t *t,
                        void *(*fn)(void *), void *arg)
{
    return pthread_create(t, NULL, fn, arg);
}

void ably_thread_join(ably_thread_t t)
{
    pthread_join(t, NULL);
}

#endif /* _WIN32 */
