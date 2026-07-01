/*
 * rabbitmq-c win32 threads — NT5.2(XP/2003) 호환 구현 (legacy32 빌드).
 * SRWLock(Vista+) 대신 CRITICAL_SECTION 사용. 정적 초기화(PTHREAD_MUTEX_INITIALIZER={0})
 * 와 동적 초기화(pthread_mutex_init) 둘 다 지원하도록 state 플래그로 lazy-init.
 * 사용 API(InitializeCriticalSection, EnterCriticalSection, Interlocked, Sleep)는 전부 NT3.1+.
 */
#include "threads.h"

#include <stdlib.h>

DWORD pthread_self(void) { return GetCurrentThreadId(); }

/* 정적 초기화된 mutex(state==0)를 처음 사용할 때 한 번만 CRITICAL_SECTION 초기화. */
static void amqp_cs_ensure(pthread_mutex_t *m) {
  if (InterlockedCompareExchange(&m->state, 1, 0) == 0) {
    InitializeCriticalSection(&m->cs);
    InterlockedExchange(&m->state, 2);
  } else {
    while (InterlockedCompareExchange(&m->state, 2, 2) != 2) {
      Sleep(0);
    }
  }
}

int pthread_mutex_init(pthread_mutex_t *mutex, void *attr) {
  (void)attr;
  if (!mutex) {
    return 1;
  }
  InitializeCriticalSection(&mutex->cs);
  InterlockedExchange(&mutex->state, 2);
  return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (!mutex) {
    return 1;
  }
  amqp_cs_ensure(mutex);
  EnterCriticalSection(&mutex->cs);
  return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  if (!mutex) {
    return 1;
  }
  LeaveCriticalSection(&mutex->cs);
  return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
  if (mutex && InterlockedCompareExchange(&mutex->state, 0, 2) == 2) {
    DeleteCriticalSection(&mutex->cs);
  }
  return 0;
}
