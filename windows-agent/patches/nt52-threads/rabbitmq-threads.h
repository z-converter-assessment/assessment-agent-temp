/*
 * rabbitmq-c win32 threads — NT5.2(XP/2003) 호환 패치 (legacy32 빌드).
 * 원본은 MinGW 에서 WINVER 을 0x0600 으로 강제하고 SRWLOCK(Vista+) 을 mutex 로 썼으나,
 * NT5.2 에는 SRWLock API 가 없어 링크 실패(undefined reference). CRITICAL_SECTION(NT3.1+)
 * 으로 대체하고, 정적 초기화자(PTHREAD_MUTEX_INITIALIZER) 호환을 위해 lazy-init 구조체를 둔다.
 */
#ifndef AMQP_THREAD_H
#define AMQP_THREAD_H

#if defined(__MINGW32__) || defined(__MINGW64__)
#ifdef WINVER
#undef WINVER
#endif
#define WINVER 0x0502
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct {
  volatile LONG state; /* 0=uninit, 1=initializing, 2=ready */
  CRITICAL_SECTION cs;
} pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER {0}

DWORD pthread_self(void);

int pthread_mutex_init(pthread_mutex_t *, void *attr);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_mutex_destroy(pthread_mutex_t *);

#endif /* AMQP_THREAD_H */
