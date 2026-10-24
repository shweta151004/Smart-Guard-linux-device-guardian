# Smart Guard Testing and Validation

Smart Guard includes subsystem and integration tests under the `tests/`
directory.

## Test Areas

The available test suite covers several parts of the platform:

- HTTPS and REST behavior
- telemetry collection
- MJPEG streaming
- camera operation
- image processing
- MQTT communication
- thermal management
- watchdog behavior
- recovery mechanisms
- end-to-end communication

## Test Structure

Tests are organized into multiple sections under:

tests/

The repository also contains standalone tests for selected native components.

## Portable Test Paths

Public test scripts resolve the project root dynamically instead of depending
on a developer-specific absolute filesystem path.

This makes the test suite portable across cloned repositories.

## Hardware-Dependent Tests

Some tests require external resources such as:

- a V4L2-compatible camera
- running Smart Guard services
- a Mosquitto broker
- TLS certificates
- MobileNetSSD model weights

## Runtime Configuration

Deployment-specific values should be provided through environment variables or
external configuration files.

Passwords and credentials are not stored directly in the test source.

## Reproducibility

Generated binaries, runtime databases, private keys, credentials, and
third-party model weights are not committed.

The repository preserves source code, configuration templates, and test logic
needed to reproduce the system behavior.
