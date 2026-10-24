#define _POSIX_C_SOURCE 200809L

#include "frame_heartbeat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define HEARTBEAT_WRITE_INTERVAL_MS 500LL
#define HEARTBEAT_CONTENT_SIZE 512

static char configured_path[PATH_MAX];
static int heartbeat_initialized = 0;

static struct timespec last_write_time =
{
    .tv_sec = 0,
    .tv_nsec = 0
};

static pthread_mutex_t heartbeat_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static long long timespec_to_nanoseconds(
    const struct timespec *value
)
{
    return
        (long long)value->tv_sec *
            1000000000LL +
        (long long)value->tv_nsec;
}

static long long elapsed_milliseconds(
    const struct timespec *start,
    const struct timespec *end
)
{
    return
        (
            timespec_to_nanoseconds(end) -
            timespec_to_nanoseconds(start)
        ) / 1000000LL;
}

static int write_all(
    int file_descriptor,
    const char *data,
    size_t size
)
{
    size_t offset = 0;

    while (offset < size)
    {
        ssize_t written = write(
            file_descriptor,
            data + offset,
            size - offset
        );

        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return -1;
        }

        if (written == 0)
        {
            return -1;
        }

        offset += (size_t)written;
    }

    return 0;
}

int frame_heartbeat_initialize(
    const char *path
)
{
    if (
        path == NULL ||
        path[0] == '\0'
    )
    {
        return -1;
    }

    pthread_mutex_lock(&heartbeat_mutex);

    if (heartbeat_initialized)
    {
        pthread_mutex_unlock(
            &heartbeat_mutex
        );

        return 0;
    }

    int length = snprintf(
        configured_path,
        sizeof(configured_path),
        "%s",
        path
    );

    if (
        length < 0 ||
        (size_t)length >=
            sizeof(configured_path)
    )
    {
        configured_path[0] = '\0';

        pthread_mutex_unlock(
            &heartbeat_mutex
        );

        return -1;
    }

    if (
        unlink(configured_path) != 0 &&
        errno != ENOENT
    )
    {
        configured_path[0] = '\0';

        pthread_mutex_unlock(
            &heartbeat_mutex
        );

        return -1;
    }

    last_write_time.tv_sec = 0;
    last_write_time.tv_nsec = 0;

    heartbeat_initialized = 1;

    printf(
        "Frame heartbeat initialized: %s\n",
        configured_path
    );

    pthread_mutex_unlock(&heartbeat_mutex);

    return 0;
}

int frame_heartbeat_record(
    unsigned long long frame_sequence
)
{
    if (frame_sequence == 0)
    {
        return -1;
    }

    struct timespec monotonic_time;
    struct timespec realtime;

    if (
        clock_gettime(
            CLOCK_MONOTONIC,
            &monotonic_time
        ) != 0 ||
        clock_gettime(
            CLOCK_REALTIME,
            &realtime
        ) != 0
    )
    {
        return -1;
    }

    pthread_mutex_lock(&heartbeat_mutex);

    if (!heartbeat_initialized)
    {
        pthread_mutex_unlock(
            &heartbeat_mutex
        );

        return -1;
    }

    if (
        last_write_time.tv_sec != 0 &&
        elapsed_milliseconds(
            &last_write_time,
            &monotonic_time
        ) < HEARTBEAT_WRITE_INTERVAL_MS
    )
    {
        pthread_mutex_unlock(
            &heartbeat_mutex
        );

        return 0;
    }

    char temporary_path[
        PATH_MAX + 64
    ];

    int path_length = snprintf(
        temporary_path,
        sizeof(temporary_path),
        "%s.tmp.%ld",
        configured_path,
        (long)getpid()
    );

    if (
        path_length < 0 ||
        (size_t)path_length >=
            sizeof(temporary_path)
    )
    {
        pthread_mutex_unlock(
            &heartbeat_mutex
        );

        return -1;
    }

    char content[HEARTBEAT_CONTENT_SIZE];

    int content_length = snprintf(
        content,
        sizeof(content),
        "version=1\n"
        "pid=%ld\n"
        "sequence=%llu\n"
        "monotonic_ns=%lld\n"
        "realtime_unix=%lld\n",
        (long)getpid(),
        frame_sequence,
        timespec_to_nanoseconds(
            &monotonic_time
        ),
        (long long)realtime.tv_sec
    );

    if (
        content_length < 0 ||
        (size_t)content_length >=
            sizeof(content)
    )
    {
        pthread_mutex_unlock(
            &heartbeat_mutex
        );

        return -1;
    }

    int file_descriptor = open(
        temporary_path,
        O_WRONLY |
        O_CREAT |
        O_TRUNC |
        O_CLOEXEC,
        0600
    );

    if (file_descriptor < 0)
    {
        pthread_mutex_unlock(
            &heartbeat_mutex
        );

        return -1;
    }

    int result = write_all(
        file_descriptor,
        content,
        (size_t)content_length
    );

    if (close(file_descriptor) != 0)
    {
        result = -1;
    }

    if (
        result == 0 &&
        rename(
            temporary_path,
            configured_path
        ) != 0
    )
    {
        result = -1;
    }

    if (result != 0)
    {
        unlink(temporary_path);

        pthread_mutex_unlock(
            &heartbeat_mutex
        );

        return -1;
    }

    last_write_time = monotonic_time;

    pthread_mutex_unlock(&heartbeat_mutex);

    return 0;
}

void frame_heartbeat_shutdown(void)
{
    pthread_mutex_lock(&heartbeat_mutex);

    if (heartbeat_initialized)
    {
        if (
            unlink(configured_path) != 0 &&
            errno != ENOENT
        )
        {
            fprintf(
                stderr,
                "Warning: frame heartbeat file "
                "could not be removed.\n"
            );
        }
    }

    configured_path[0] = '\0';
    heartbeat_initialized = 0;

    last_write_time.tv_sec = 0;
    last_write_time.tv_nsec = 0;

    pthread_mutex_unlock(&heartbeat_mutex);
}
