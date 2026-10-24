#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "camera_stream.h"
#include "email_alert.h"
#include "frame_heartbeat.h"
#include "detection_history.h"
#include "guard_mode.h"
#include "image_processor.h"
#include "thermal_manager.h"
#include "web_server.h"

#define HTTP_PORT 8080
#define HTTPS_PORT 8443

#define INDEX_FILE "web/index.html"
#define CERTIFICATE_FILE "certs/server.crt"
#define PRIVATE_KEY_FILE "certs/server.key"

#define CAMERA_DEVICE "/dev/video0"
#define CAMERA_WIDTH 640
#define CAMERA_HEIGHT 480
#define CAMERA_FPS 30

#define MODEL_CONFIG \
    "image_processing/models/MobileNetSSD_deploy.prototxt"

#define MODEL_WEIGHTS \
    "image_processing/models/MobileNetSSD_deploy.caffemodel"

#define STUDENT_ID "device-001"

#define DEFAULT_HEARTBEAT_FILE \
    "/run/smart-guard/frame_heartbeat"

static volatile sig_atomic_t keep_running = 1;

static void handle_shutdown_signal(
    int signal_number
)
{
    (void)signal_number;
    keep_running = 0;
}

int main(void)
{
    signal(
        SIGINT,
        handle_shutdown_signal
    );

    signal(
        SIGTERM,
        handle_shutdown_signal
    );

    printf("========================================\n");
    printf(" Smart Guard Monitoring System\n");
    printf(" Student: Saina Kashani\n");
    printf(" Student ID: %s\n", STUDENT_ID);
    printf("========================================\n");

    const char *heartbeat_path =
        getenv(
            "SMART_GUARD_HEARTBEAT_PATH"
        );

    if (
        heartbeat_path == NULL ||
        heartbeat_path[0] == '\0'
    )
    {
        heartbeat_path =
            DEFAULT_HEARTBEAT_FILE;
    }

    if (
        frame_heartbeat_initialize(
            heartbeat_path
        ) != 0
    )
    {
        fprintf(
            stderr,
            "Error: frame heartbeat "
            "initialization failed.\n"
        );

        return EXIT_FAILURE;
    }

    if (
        atexit(
            frame_heartbeat_shutdown
        ) != 0
    )
    {
        fprintf(
            stderr,
            "Error: frame heartbeat cleanup "
            "registration failed.\n"
        );

        frame_heartbeat_shutdown();

        return EXIT_FAILURE;
    }

    if (
        detection_history_initialize(
            "data/detection_history.db"
        ) != 0
    )
    {
        fprintf(
            stderr,
            "Error: detection history initialization failed.\n"
        );

        return EXIT_FAILURE;
    }

    if (
        atexit(
            detection_history_shutdown
        ) != 0
    )
    {
        fprintf(
            stderr,
            "Error: detection history cleanup registration failed.\n"
        );

        detection_history_shutdown();

        return EXIT_FAILURE;
    }

    if (
        guard_mode_initialize(
            "data/guard_mode.state"
        ) != 0
    )
    {
        fprintf(
            stderr,
            "Error: guard mode initialization failed.\n"
        );

        return EXIT_FAILURE;
    }

    if (email_alert_initialize() != 0)
    {
        fprintf(
            stderr,
            "Warning: email alert module is unavailable.\n"
        );
    }

    if (
        thermal_manager_initialize() != 0
    )
    {
        fprintf(
            stderr,
            "Error: thermal manager initialization failed.\n"
        );

        email_alert_shutdown();
        guard_mode_shutdown();

        return EXIT_FAILURE;
    }

    if (
        atexit(
            thermal_manager_shutdown
        ) != 0
    )
    {
        fprintf(
            stderr,
            "Error: thermal manager cleanup "
            "registration failed.\n"
        );

        thermal_manager_shutdown();
        email_alert_shutdown();
        guard_mode_shutdown();

        return EXIT_FAILURE;
    }


    if (
        image_processor_initialize(
            MODEL_CONFIG,
            MODEL_WEIGHTS,
            STUDENT_ID
        ) != 0
    )
    {
        fprintf(
            stderr,
            "Error: image processor initialization failed.\n"
        );

        email_alert_shutdown();

        guard_mode_shutdown();

        return EXIT_FAILURE;
    }

    printf(
        "Image processor initialized successfully.\n"
    );

    if (
        camera_stream_start(
            CAMERA_DEVICE,
            CAMERA_WIDTH,
            CAMERA_HEIGHT,
            CAMERA_FPS
        ) != 0
    )
    {
        fprintf(
            stderr,
            "Error: camera could not start.\n"
        );

        image_processor_shutdown();

        email_alert_shutdown();

        guard_mode_shutdown();

        return EXIT_FAILURE;
    }

    printf(
        "Camera started successfully: %s\n",
        CAMERA_DEVICE
    );

    struct MHD_Daemon *https_daemon =
        web_server_start_https(
            HTTPS_PORT,
            INDEX_FILE,
            CERTIFICATE_FILE,
            PRIVATE_KEY_FILE
        );

    if (https_daemon == NULL)
    {
        fprintf(
            stderr,
            "Error: HTTPS web server could not start.\n"
        );

        camera_stream_stop();
        image_processor_shutdown();

        email_alert_shutdown();

        guard_mode_shutdown();

        return EXIT_FAILURE;
    }

    printf(
        "HTTPS web server started successfully.\n"
        "Secure URL: https://127.0.0.1:%d\n",
        HTTPS_PORT
    );

    struct MHD_Daemon *redirect_daemon =
        web_server_start_redirect(
            HTTP_PORT,
            HTTPS_PORT
        );

    if (redirect_daemon == NULL)
    {
        fprintf(
            stderr,
            "Error: HTTP redirect server could not start.\n"
        );

        web_server_stop(https_daemon);
        web_server_tls_cleanup();
        camera_stream_stop();
        image_processor_shutdown();

        email_alert_shutdown();

        guard_mode_shutdown();

        return EXIT_FAILURE;
    }

    printf(
        "HTTP redirect server started successfully.\n"
        "Redirect URL: http://127.0.0.1:%d\n"
        "Press Ctrl+C to stop the application.\n",
        HTTP_PORT
    );

    while (keep_running)
    {
        sleep(1);
    }

    printf("\nStopping HTTP redirect server...\n");

    web_server_stop(redirect_daemon);

    printf("Stopping HTTPS web server...\n");

    web_server_stop(https_daemon);
    web_server_tls_cleanup();

    printf("Stopping camera...\n");

    camera_stream_stop();

    printf("Stopping image processor...\n");

    image_processor_shutdown();

    email_alert_shutdown();

    guard_mode_shutdown();

    printf("Application stopped successfully.\n");

    return EXIT_SUCCESS;
}
