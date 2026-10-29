#define _GNU_SOURCE
#include "faults.h"

#include <dlfcn.h>
#include <errno.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* All interposition is confined to an explicitly preloaded native child. */
static atomic_int armed;
static atomic_int flags;
static atomic_int failure_thread;
static atomic_long counters[FaultCounterCount];
static pthread_mutex_t barrier_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier_changed = PTHREAD_COND_INITIALIZER;
static int released;
static pthread_key_t tls_key;
static pthread_once_t tls_once = PTHREAD_ONCE_INIT;
static int tls_error;
static char watched_path[4096];
static _Atomic(FILE *) watched_stream;
static atomic_uintptr_t last_destroyed_handle;
static atomic_uintptr_t latest_helper_handle;

int mpvqml_fault_watch_file(const char *path)
{
    if (!path || strlen(path) >= sizeof(watched_path))
        return EINVAL;
    pthread_mutex_lock(&barrier_mutex);
    strcpy(watched_path, path);
    pthread_mutex_unlock(&barrier_mutex);
    return 0;
}

static const char *const mpv_symbols[] = {
    "mpv_create", "mpv_initialize", "mpv_destroy", "mpv_terminate_destroy"
};
static void *bound_symbols[4];
static atomic_int scope_bound;

int mpvqml_fault_bind_scope(void *consumer)
{
    if (!consumer || atomic_load(&scope_bound))
        return EINVAL;
    Dl_info fixture;
    if (!dladdr((void *)&mpvqml_fault_bind_scope, &fixture))
        return EINVAL;
    dlerror();
    void *anchor = dlsym(consumer, "mpv_error_string");
    const char *error = dlerror();
    Dl_info libmpv;
    if (error || !anchor || !dladdr(anchor, &libmpv)
        || !libmpv.dli_fbase || libmpv.dli_fbase == fixture.dli_fbase)
        return EINVAL;
    void *resolved[4];
    for (int i = 0; i < 4; ++i) {
        dlerror();
        resolved[i] = dlsym(consumer, mpv_symbols[i]);
        error = dlerror();
        Dl_info identity;
        if (error || !resolved[i] || !dladdr(resolved[i], &identity)
            || identity.dli_fbase != libmpv.dli_fbase)
            return EINVAL;
    }
    memcpy(bound_symbols, resolved, sizeof(resolved));
    atomic_store_explicit(&scope_bound, 1, memory_order_release);
    /* No consumer handle/loader reference is retained. Production pinning
       must keep these checked libmpv addresses valid after consumer unload. */
    return 0;
}

static void *required_symbol(const char *name)
{
    if (atomic_load_explicit(&scope_bound, memory_order_acquire)) {
        for (int i = 0; i < 4; ++i)
            if (strcmp(name, mpv_symbols[i]) == 0)
                return bound_symbols[i];
    }
    dlerror();
    void *symbol = dlsym(RTLD_NEXT, name);
    if (dlerror() || !symbol)
        _exit(90);
    return symbol;
}

void mpvqml_fault_configure(int faults, int fail_thread)
{
    atomic_store(&flags, faults);
    atomic_store(&failure_thread, fail_thread);
    atomic_store(&armed, 1);
}

long mpvqml_fault_counter(int counter)
{
    return counter >= 0 && counter < FaultCounterCount
        ? atomic_load(&counters[counter]) : -1;
}

static void await_release(int entered, int completed)
{
    pthread_mutex_lock(&barrier_mutex);
    atomic_fetch_add(&counters[entered], 1);
    while (!released)
        pthread_cond_wait(&barrier_changed, &barrier_mutex);
    pthread_mutex_unlock(&barrier_mutex);
    if (completed >= 0)
        atomic_fetch_add(&counters[completed], 1);
}

void mpvqml_fault_release(void)
{
    pthread_mutex_lock(&barrier_mutex);
    released = 1;
    pthread_cond_broadcast(&barrier_changed);
    pthread_mutex_unlock(&barrier_mutex);
}

static int watch_open(const char *path)
{
    pthread_mutex_lock(&barrier_mutex);
    const int matches = watched_path[0] && strcmp(watched_path, path) == 0;
    pthread_mutex_unlock(&barrier_mutex);
    if (matches)
        atomic_fetch_add(&counters[FaultFileOpens], 1);
    return matches;
}

FILE *fopen(const char *path, const char *mode)
{
    FILE *(*real_open)(const char *, const char *) = required_symbol("fopen");
    const int watched = watch_open(path);
    FILE *stream = real_open(path, mode);
    if (watched && stream)
        atomic_store(&watched_stream, stream);
    return stream;
}

FILE *fopen64(const char *path, const char *mode)
{
    FILE *(*real_open)(const char *, const char *) = required_symbol("fopen64");
    const int watched = watch_open(path);
    FILE *stream = real_open(path, mode);
    if (watched && stream)
        atomic_store(&watched_stream, stream);
    return stream;
}

size_t fwrite(const void *data, size_t size, size_t count, FILE *stream)
{
    size_t (*real_write)(const void *, size_t, size_t, FILE *) = required_symbol("fwrite");
    const int watched = stream == atomic_load(&watched_stream);
    if (watched) {
        if (atomic_load(&flags) & FaultFileBarrier)
            await_release(FaultFileWrites, -1);
        else
            atomic_fetch_add(&counters[FaultFileWrites], 1);
    }
    const size_t result = real_write(data, size, count, stream);
    if (watched && result == count)
        atomic_fetch_add(&counters[FaultFileWritesCompleted], 1);
    return result;
}

int fclose(FILE *stream)
{
    int (*real_close)(FILE *) = required_symbol("fclose");
    FILE *expected = stream;
    atomic_compare_exchange_strong(&watched_stream, &expected, NULL);
    return real_close(stream);
}

static void tls_destructor(void *value)
{
    if (value && (atomic_load(&flags) & FaultTlsBarrier))
        await_release(FaultTlsEntered, FaultTlsCompleted);
}

static void create_tls_key(void)
{
    tls_error = pthread_key_create(&tls_key, tls_destructor);
}

int mpvqml_fault_seed_tls(void)
{
    int error = pthread_once(&tls_once, create_tls_key);
    if (error || tls_error)
        return error ? error : tls_error;
    return pthread_setspecific(tls_key, &tls_key);
}

mpv_handle *mpv_create(void)
{
    mpv_handle *(*real_create)(void) = required_symbol("mpv_create");
    atomic_fetch_add(&counters[FaultCreates], 1);
    if (atomic_load(&flags) & FaultCreate)
        return NULL;
    return real_create();
}

int mpv_initialize(mpv_handle *handle)
{
    int (*real_initialize)(mpv_handle *) = required_symbol("mpv_initialize");
    atomic_fetch_add(&counters[FaultInitializes], 1);
    if (atomic_load(&flags) & FaultInitialize)
        return MPV_ERROR_INVALID_PARAMETER;
    return real_initialize(handle);
}

void mpv_destroy(mpv_handle *handle)
{
    void (*real_destroy)(mpv_handle *) = required_symbol("mpv_destroy");
    atomic_fetch_add(&counters[FaultCandidateDestroys], 1);
    atomic_store(&last_destroyed_handle, (uintptr_t)handle);
    real_destroy(handle);
}

mpv_handle *mpv_create_client(mpv_handle *core, const char *name)
{
    mpv_handle *(*real_create)(mpv_handle *, const char *) = required_symbol("mpv_create_client");
    mpv_handle *client = real_create(core, name);
    if (client) {
        atomic_store(&latest_helper_handle, (uintptr_t)client);
        atomic_fetch_add(&counters[FaultHelperClients], 1);
        if ((uintptr_t)client == atomic_load(&last_destroyed_handle))
            atomic_fetch_add(&counters[FaultHelperAddressReuses], 1);
    }
    return client;
}

mpv_event *mpv_wait_event(mpv_handle *handle, double timeout)
{
    mpv_event *(*real_wait)(mpv_handle *, double) = required_symbol("mpv_wait_event");
    mpv_event *event = real_wait(handle, timeout);
    if ((atomic_load(&flags) & FaultSynchronousWakeup)
        && (uintptr_t)handle == atomic_load(&latest_helper_handle)
        && event && event->event_id == MPV_EVENT_NONE)
        atomic_fetch_add(&counters[FaultHelperEmptyDrains], 1);
    return event;
}

void mpv_terminate_destroy(mpv_handle *handle)
{
    void (*real_destroy)(mpv_handle *) = required_symbol("mpv_terminate_destroy");
    if (atomic_load(&flags) & FaultRetirementBarrier)
        await_release(FaultRetirementEntered, -1);
    else
        atomic_fetch_add(&counters[FaultRetirementEntered], 1);
    real_destroy(handle);
    atomic_fetch_add(&counters[FaultRetirementCompleted], 1);
}

int mpv_render_context_create(mpv_render_context **context, mpv_handle *handle,
                             mpv_render_param *parameters)
{
    int (*real_create)(mpv_render_context **, mpv_handle *, mpv_render_param *)
        = required_symbol("mpv_render_context_create");
    atomic_fetch_add(&counters[FaultRenderCreates], 1);
    if (atomic_load(&flags) & FaultRenderCreate)
        return MPV_ERROR_UNSUPPORTED;
    return real_create(context, handle, parameters);
}

void mpv_set_wakeup_callback(mpv_handle *handle, void (*callback)(void *), void *context)
{
    void (*real_register)(mpv_handle *, void (*)(void *), void *)
        = required_symbol("mpv_set_wakeup_callback");
    if (callback && (atomic_load(&flags) & FaultSynchronousWakeup)) {
        atomic_fetch_add(&counters[FaultSynchronousRegistrations], 1);
        callback(context);
    }
    real_register(handle, callback, context);
}

struct BootstrapStart {
    void *(*function)(void *);
    void *argument;
};

static void *bootstrap_start(void *argument)
{
    const struct BootstrapStart start = *(struct BootstrapStart *)argument;
    free(argument);
    void *result = start.function(start.argument);
    atomic_fetch_add(&counters[FaultBootstrapThreadsFinished], 1);
    return result;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                   void *(*function)(void *), void *argument)
{
    int (*real_create)(pthread_t *, const pthread_attr_t *,
                       void *(*)(void *), void *) = required_symbol("pthread_create");
    if (!atomic_load(&armed))
        return real_create(thread, attributes, function, argument);
    const long attempt = atomic_fetch_add(&counters[FaultThreadAttempts], 1) + 1;
    if (attempt == atomic_load(&failure_thread)) {
        atomic_fetch_add(&counters[FaultThreadFailures], 1);
        return EAGAIN;
    }
    /* The first two armed threads are the production retirement bootstrap. */
    if (attempt <= 2) {
        struct BootstrapStart *start = malloc(sizeof(*start));
        if (!start)
            return ENOMEM;
        *start = (struct BootstrapStart){function, argument};
        int error = real_create(thread, attributes, bootstrap_start, start);
        if (error)
            free(start);
        return error;
    }
    return real_create(thread, attributes, function, argument);
}
