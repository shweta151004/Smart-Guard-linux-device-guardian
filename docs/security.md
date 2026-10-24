# Smart Guard Security Design

Smart Guard separates source code from deployment credentials and uses several
Linux security mechanisms to reduce exposure.

## Credential Management

Production credentials are not stored in the repository.

Runtime configuration is supplied using external environment files.

Examples include:

- email credentials
- MQTT credentials
- thermal configuration
- watchdog configuration

Only safe example configuration files are included in source control.

## HTTPS / TLS

Smart Guard supports TLS-protected HTTPS communication.

Private TLS keys and generated deployment certificates are intentionally
excluded from the repository.

The repository contains certificate configuration and helper scripts only.

## MQTT Security

MQTT deployments can use authenticated broker access.

Usernames and passwords are supplied externally rather than being embedded in
the source code.

## Dedicated Service Account

The main Smart Guard service is designed to run using a dedicated
`smartguard` system account instead of a normal developer account.

## systemd Hardening

Service definitions include hardening options where appropriate, including:

- NoNewPrivileges=true
- PrivateTmp=true
- ProtectKernelTunables=true
- ProtectKernelModules=true
- ProtectControlGroups=true
- RestrictSUIDSGID=true
- LockPersonality=true

## Privileged Operations

Operations that require elevated permissions are separated from the main
application where possible.

Controlled reboot functionality is implemented through a dedicated systemd
service and polkit rule.

## Repository Hygiene

The public repository excludes:

- private TLS keys
- real environment files
- SQLite runtime databases
- generated binaries
- Python virtual environments
- source-code backup files
- third-party model weights
- temporary runtime state
