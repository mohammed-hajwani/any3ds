/* Standalone regression for the Windows rasteriser crash. Build with the
 * same static MinGW link as tools/build_win.sh, including -femulated-tls.
 * Without that flag, Clang's native TLS access faults before rasterising.
 * No GPU or game data is needed. */
#include <pthread.h>
#include <stdio.h>

static int shared;
static _Thread_local int *counter = &shared;

static void *worker(void *arg)
{
    int local;
    if (counter != &shared)
        return (void *)1;
    counter = &local;
    *(int *)arg = counter == &local;
    return NULL;
}

int main(void)
{
    int local, separate = 0;
    pthread_t thread;
    void *result;
    if (counter != &shared)
        return 1;
    counter = &local;
    if (pthread_create(&thread, NULL, worker, &separate))
        return 2;
    if (pthread_join(thread, &result))
        return 2;
    if (result || !separate || counter != &local)
        return 1;
    puts("PASS: initialized TLS is writable and isolated across pthreads");
    return 0;
}
