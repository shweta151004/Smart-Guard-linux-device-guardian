# Smart Guard Architecture

Smart Guard is organized as a set of cooperating Linux services and native
components for real-time monitoring, computer vision, telemetry, messaging,
storage, and recovery.

## Main Application

The core C application coordinates:

- V4L2 camera capture
- image processing
- telemetry collection
- HTTPS services
- detection history
- email alerts
- guard mode
- thermal management
- frame heartbeat generation

## Camera Pipeline

A Linux V4L2 camera provides frames to the Smart Guard processing pipeline.

Frames are used for:

1. live MJPEG streaming
2. OpenCV-based object detection

## Image Processing

The image-processing module uses OpenCV DNN with MobileNetSSD.

Detected people are counted and the result is made available to other
subsystems such as MQTT, event logging, and alert generation.

## Web Layer

The native HTTPS server is implemented with libmicrohttpd.

It provides access to:

- dashboard content
- system telemetry
- detection history
- video streaming
- guard-mode functionality

A separate FastAPI application provides an additional API and Swagger
interface.

## Telemetry

Linux system information is collected from standard system interfaces such as
/proc and /sys.

The telemetry subsystem provides information such as:

- CPU temperature
- CPU utilization
- available memory

## MQTT

The MQTT client communicates with a Mosquitto broker and publishes runtime
information such as telemetry, status, and person-count data.

## Persistent Storage

SQLite is used to preserve detection history across process and service
restarts.

## Reliability

Smart Guard combines multiple reliability mechanisms:

- systemd process supervision
- graceful shutdown handling
- frame-heartbeat generation
- external watchdog monitoring
- automatic recovery
- persistent state

## Thermal Management

The thermal manager monitors processor temperature and can reduce processing
load when the system becomes too hot.

When the configured recovery condition is reached, normal processing can be
restored.

## Deployment

The public deployment layout assumes:

/opt/smart-guard/

Runtime configuration is stored separately under:

/etc/smart-guard/

This separates application source and binaries from credentials and
deployment-specific configuration.
