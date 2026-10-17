#include "startup_injection.hpp"

#include <cerrno>
#include <pthread.h>

using namespace ludus::foundation::threading::startup_test;

// GNU linker --wrap requires these exact external symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier,readability-identifier-naming)
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __real_pthread_join(pthread_t, void**);
extern "C" int __real_pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*);
extern "C" int __real_pthread_cond_init(pthread_cond_t*, const pthread_condattr_t*);

extern "C" int
__wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attributes, void* (*entry)(void*), void* context)
{
    if (CountStartAndCheckFailure() == true)
    {
        return EAGAIN;
    }
    return __real_pthread_create(thread, attributes, entry, context);
}
extern "C" int __wrap_pthread_join(pthread_t thread, void** result)
{
    ++gJoins;
    return __real_pthread_join(thread, result);
}
extern "C" int __wrap_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attributes)
{
    return shouldFailMutex == true ? EAGAIN : __real_pthread_mutex_init(mutex, attributes);
}
extern "C" int __wrap_pthread_cond_init(pthread_cond_t* condition, const pthread_condattr_t* attributes)
{
    return shouldFailCondition == true ? EAGAIN : __real_pthread_cond_init(condition, attributes);
}

// NOLINTEND(bugprone-reserved-identifier,readability-identifier-naming)
