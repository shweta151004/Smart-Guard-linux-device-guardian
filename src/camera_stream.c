#define _POSIX_C_SOURCE 200809L

#include "camera_stream.h"
#include "frame_heartbeat.h"
#include "thermal_manager.h"
#include "image_processor.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
    void *address;
    size_t length;
} MappedBuffer;

typedef struct
{
    int fd;
    MappedBuffer *buffers;
    unsigned int buffer_count;
    pthread_t thread;
    int thread_started;
    int running;
    unsigned char *latest_frame;
    size_t latest_frame_size;
    unsigned long long sequence;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
} CameraState;

static CameraState camera_state =
{
    .fd = -1,
    .buffers = NULL,
    .buffer_count = 0,
    .thread_started = 0,
    .running = 0,
    .latest_frame = NULL,
    .latest_frame_size = 0,
    .sequence = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER
};

static int ioctl_retry(
    int fd,
    unsigned long request,
    void *argument
)
{
    int result;

    do
    {
        result = ioctl(fd, request, argument);
    }
    while (result == -1 && errno == EINTR);

    return result;
}

static int store_latest_frame(
    const unsigned char *frame_data,
    size_t frame_size
)
{
    if (
        frame_data == NULL ||
        frame_size == 0
    )
    {
        return -1;
    }

    pthread_mutex_lock(&camera_state.mutex);

    unsigned char *new_frame = realloc(
        camera_state.latest_frame,
        frame_size
    );

    if (new_frame == NULL)
    {
        pthread_mutex_unlock(
            &camera_state.mutex
        );

        return -1;
    }

    camera_state.latest_frame = new_frame;

    memcpy(
        camera_state.latest_frame,
        frame_data,
        frame_size
    );

    camera_state.latest_frame_size =
        frame_size;

    camera_state.sequence++;

    unsigned long long stored_sequence =
        camera_state.sequence;

    pthread_cond_broadcast(
        &camera_state.condition
    );

    pthread_mutex_unlock(&camera_state.mutex);

    (void)frame_heartbeat_record(
        stored_sequence
    );

    return 0;
}

static void release_camera_resources(void)
{
    if (camera_state.buffers != NULL)
    {
        for (
            unsigned int index = 0;
            index < camera_state.buffer_count;
            index++
        )
        {
            if (
                camera_state.buffers[index].address != NULL &&
                camera_state.buffers[index].address != MAP_FAILED
            )
            {
                munmap(
                    camera_state.buffers[index].address,
                    camera_state.buffers[index].length
                );
            }
        }

        free(camera_state.buffers);
        camera_state.buffers = NULL;
    }

    camera_state.buffer_count = 0;

    if (camera_state.fd >= 0)
    {
        close(camera_state.fd);
        camera_state.fd = -1;
    }
}

static void *capture_thread(void *argument)
{
    (void)argument;

    while (1)
    {
        pthread_mutex_lock(&camera_state.mutex);

        int running = camera_state.running;

        pthread_mutex_unlock(&camera_state.mutex);

        if (!running)
        {
            break;
        }

        struct pollfd descriptor;

        descriptor.fd = camera_state.fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;

        int poll_result = poll(
            &descriptor,
            1,
            1000
        );

        if (poll_result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            break;
        }

        if (
            poll_result == 0 ||
            (descriptor.revents & POLLIN) == 0
        )
        {
            continue;
        }

        struct v4l2_buffer buffer;

        memset(&buffer, 0, sizeof(buffer));

        buffer.type =
            V4L2_BUF_TYPE_VIDEO_CAPTURE;

        buffer.memory =
            V4L2_MEMORY_MMAP;

        if (
            ioctl_retry(
                camera_state.fd,
                VIDIOC_DQBUF,
                &buffer
            ) < 0
        )
        {
            if (errno == EAGAIN)
            {
                continue;
            }

            break;
        }

        int should_process_frame =
            thermal_manager_should_process_frame();

        unsigned char *raw_frame = NULL;
        size_t raw_frame_size = 0;

        if (
            should_process_frame &&
            buffer.index < camera_state.buffer_count &&
            buffer.bytesused > 0
        )
        {
            raw_frame_size = buffer.bytesused;

            raw_frame = malloc(
                raw_frame_size
            );

            if (raw_frame != NULL)
            {
                memcpy(
                    raw_frame,
                    camera_state.buffers[
                        buffer.index
                    ].address,
                    raw_frame_size
                );
            }
        }

        if (
            ioctl_retry(
                camera_state.fd,
                VIDIOC_QBUF,
                &buffer
            ) < 0
        )
        {
            free(raw_frame);
            break;
        }

        if (!should_process_frame)
        {
            continue;
        }

        if (raw_frame == NULL)
        {
            continue;
        }

        unsigned char *processed_frame = NULL;
        size_t processed_frame_size = 0;

        int process_result =
            image_processor_process_jpeg(
                raw_frame,
                raw_frame_size,
                &processed_frame,
                &processed_frame_size
            );

        if (
            process_result == 0 &&
            processed_frame != NULL &&
            processed_frame_size > 0
        )
        {
            store_latest_frame(
                processed_frame,
                processed_frame_size
            );

            free(processed_frame);
        }
        else
        {
            store_latest_frame(
                raw_frame,
                raw_frame_size
            );
        }

        free(raw_frame);
    }

    pthread_mutex_lock(&camera_state.mutex);

    camera_state.running = 0;

    pthread_cond_broadcast(
        &camera_state.condition
    );

    pthread_mutex_unlock(&camera_state.mutex);

    return NULL;
}

int camera_stream_start(
    const char *device,
    unsigned int width,
    unsigned int height,
    unsigned int fps
)
{
    if (
        device == NULL ||
        width == 0 ||
        height == 0 ||
        fps == 0
    )
    {
        return -1;
    }

    if (camera_state.fd >= 0)
    {
        return -1;
    }

    camera_state.fd = open(
        device,
        O_RDWR | O_NONBLOCK
    );

    if (camera_state.fd < 0)
    {
        perror("open camera");
        return -1;
    }

    struct v4l2_capability capability;

    memset(&capability, 0, sizeof(capability));

    if (
        ioctl_retry(
            camera_state.fd,
            VIDIOC_QUERYCAP,
            &capability
        ) < 0
    )
    {
        perror("VIDIOC_QUERYCAP");
        release_camera_resources();
        return -1;
    }

    uint32_t capabilities =
        capability.capabilities;

    if (
        capability.capabilities &
        V4L2_CAP_DEVICE_CAPS
    )
    {
        capabilities =
            capability.device_caps;
    }

    if (
        (capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 ||
        (capabilities & V4L2_CAP_STREAMING) == 0
    )
    {
        release_camera_resources();
        return -1;
    }

    struct v4l2_format format;

    memset(&format, 0, sizeof(format));

    format.type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE;

    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat =
        V4L2_PIX_FMT_MJPEG;

    format.fmt.pix.field =
        V4L2_FIELD_ANY;

    if (
        ioctl_retry(
            camera_state.fd,
            VIDIOC_S_FMT,
            &format
        ) < 0
    )
    {
        perror("VIDIOC_S_FMT");
        release_camera_resources();
        return -1;
    }

    if (
        format.fmt.pix.pixelformat !=
        V4L2_PIX_FMT_MJPEG
    )
    {
        release_camera_resources();
        return -1;
    }

    struct v4l2_streamparm parameters;

    memset(&parameters, 0, sizeof(parameters));

    parameters.type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE;

    parameters.parm.capture.timeperframe.numerator =
        1;

    parameters.parm.capture.timeperframe.denominator =
        fps;

    ioctl_retry(
        camera_state.fd,
        VIDIOC_S_PARM,
        &parameters
    );

    struct v4l2_requestbuffers request;

    memset(&request, 0, sizeof(request));

    request.count = 4;
    request.type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE;

    request.memory =
        V4L2_MEMORY_MMAP;

    if (
        ioctl_retry(
            camera_state.fd,
            VIDIOC_REQBUFS,
            &request
        ) < 0 ||
        request.count < 2
    )
    {
        perror("VIDIOC_REQBUFS");
        release_camera_resources();
        return -1;
    }

    camera_state.buffers = calloc(
        request.count,
        sizeof(MappedBuffer)
    );

    if (camera_state.buffers == NULL)
    {
        release_camera_resources();
        return -1;
    }

    camera_state.buffer_count =
        request.count;

    for (
        unsigned int index = 0;
        index < camera_state.buffer_count;
        index++
    )
    {
        struct v4l2_buffer buffer;

        memset(&buffer, 0, sizeof(buffer));

        buffer.type =
            V4L2_BUF_TYPE_VIDEO_CAPTURE;

        buffer.memory =
            V4L2_MEMORY_MMAP;

        buffer.index = index;

        if (
            ioctl_retry(
                camera_state.fd,
                VIDIOC_QUERYBUF,
                &buffer
            ) < 0
        )
        {
            perror("VIDIOC_QUERYBUF");
            release_camera_resources();
            return -1;
        }

        camera_state.buffers[index].length =
            buffer.length;

        camera_state.buffers[index].address =
            mmap(
                NULL,
                buffer.length,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                camera_state.fd,
                buffer.m.offset
            );

        if (
            camera_state.buffers[index].address ==
            MAP_FAILED
        )
        {
            perror("mmap");
            release_camera_resources();
            return -1;
        }

        if (
            ioctl_retry(
                camera_state.fd,
                VIDIOC_QBUF,
                &buffer
            ) < 0
        )
        {
            perror("VIDIOC_QBUF");
            release_camera_resources();
            return -1;
        }
    }

    enum v4l2_buf_type type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (
        ioctl_retry(
            camera_state.fd,
            VIDIOC_STREAMON,
            &type
        ) < 0
    )
    {
        perror("VIDIOC_STREAMON");
        release_camera_resources();
        return -1;
    }

    pthread_mutex_lock(&camera_state.mutex);

    free(camera_state.latest_frame);

    camera_state.latest_frame = NULL;
    camera_state.latest_frame_size = 0;
    camera_state.sequence = 0;
    camera_state.running = 1;

    pthread_mutex_unlock(&camera_state.mutex);

    if (
        pthread_create(
            &camera_state.thread,
            NULL,
            capture_thread,
            NULL
        ) != 0
    )
    {
        pthread_mutex_lock(&camera_state.mutex);

        camera_state.running = 0;

        pthread_mutex_unlock(&camera_state.mutex);

        ioctl_retry(
            camera_state.fd,
            VIDIOC_STREAMOFF,
            &type
        );

        release_camera_resources();

        return -1;
    }

    camera_state.thread_started = 1;

    return 0;
}

void camera_stream_stop(void)
{
    if (camera_state.fd < 0)
    {
        return;
    }

    pthread_mutex_lock(&camera_state.mutex);

    camera_state.running = 0;

    pthread_cond_broadcast(
        &camera_state.condition
    );

    pthread_mutex_unlock(&camera_state.mutex);

    if (camera_state.thread_started)
    {
        pthread_join(
            camera_state.thread,
            NULL
        );

        camera_state.thread_started = 0;
    }

    enum v4l2_buf_type type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ioctl_retry(
        camera_state.fd,
        VIDIOC_STREAMOFF,
        &type
    );

    release_camera_resources();

    pthread_mutex_lock(&camera_state.mutex);

    free(camera_state.latest_frame);

    camera_state.latest_frame = NULL;
    camera_state.latest_frame_size = 0;
    camera_state.sequence = 0;

    pthread_mutex_unlock(&camera_state.mutex);
}

int camera_stream_wait_for_frame(
    unsigned long long after_sequence,
    unsigned char **frame_data,
    size_t *frame_size,
    unsigned long long *frame_sequence,
    int timeout_ms
)
{
    if (
        frame_data == NULL ||
        frame_size == NULL ||
        frame_sequence == NULL ||
        timeout_ms <= 0
    )
    {
        return -1;
    }

    struct timespec deadline;

    if (
        clock_gettime(
            CLOCK_REALTIME,
            &deadline
        ) != 0
    )
    {
        return -1;
    }

    deadline.tv_sec +=
        timeout_ms / 1000;

    deadline.tv_nsec +=
        (long)(timeout_ms % 1000) *
        1000000L;

    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&camera_state.mutex);

    while (
        camera_state.running &&
        camera_state.sequence <= after_sequence
    )
    {
        int wait_result =
            pthread_cond_timedwait(
                &camera_state.condition,
                &camera_state.mutex,
                &deadline
            );

        if (wait_result == ETIMEDOUT)
        {
            pthread_mutex_unlock(
                &camera_state.mutex
            );

            return -1;
        }
    }

    if (
        camera_state.sequence <= after_sequence ||
        camera_state.latest_frame == NULL ||
        camera_state.latest_frame_size == 0
    )
    {
        pthread_mutex_unlock(
            &camera_state.mutex
        );

        return -1;
    }

    unsigned char *copy = malloc(
        camera_state.latest_frame_size
    );

    if (copy == NULL)
    {
        pthread_mutex_unlock(
            &camera_state.mutex
        );

        return -1;
    }

    memcpy(
        copy,
        camera_state.latest_frame,
        camera_state.latest_frame_size
    );

    *frame_data = copy;

    *frame_size =
        camera_state.latest_frame_size;

    *frame_sequence =
        camera_state.sequence;

    pthread_mutex_unlock(&camera_state.mutex);

    return 0;
}
