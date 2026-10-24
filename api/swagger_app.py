from __future__ import annotations

import time
from pathlib import Path
from typing import Any

import httpx
from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import JSONResponse, RedirectResponse, Response, StreamingResponse
from pydantic import BaseModel, Field

PROJECT_ROOT = Path(__file__).resolve().parent.parent
CA_FILE = PROJECT_ROOT / "certs" / "server.crt"
BACKEND_URL = "https://127.0.0.1:8443"


class PersonsResponse(BaseModel):
    status: str
    student_id: str
    persons: int
    timestamp: str


class TelemetryResponse(BaseModel):
    status: str
    student_id: str
    cpu_temperature_c: float
    memory_available_kb: int
    memory_available_mb: float
    cpu_usage_percent: float
    timestamp: str


class HistoryRecord(BaseModel):
    persons: int
    timestamp: str


class HistoryResponse(BaseModel):
    status: str
    student_id: str
    count: int
    history: list[HistoryRecord]


class CommandRequest(BaseModel):
    cmd: str = Field(
        examples=["reboot"],
        description="Command forwarded directly to the C API"
    )


class CommandAcceptedResponse(BaseModel):
    status: str
    cmd: str
    message: str
    timestamp: str


app = FastAPI(
    title="Smart Guard RESTful API",
    summary="Swagger documentation layer for the Smart Guard C API",
    description=(
        "All monitoring, telemetry, history, image processing and command "
        "logic is implemented by the C/C++ backend. FastAPI only documents "
        "and forwards requests to that backend."
    ),
    version="2.0.0",
    docs_url="/docs",
    redoc_url=None,
    openapi_url="/openapi.json",
    contact={
        "name": "Saina Kashani",
    },
)


async def request_backend_json(
    method: str,
    path: str,
    json_body: dict[str, Any] | None = None,
) -> tuple[int, Any]:
    timeout = httpx.Timeout(
        connect=5.0,
        read=10.0,
        write=5.0,
        pool=5.0,
    )

    try:
        async with httpx.AsyncClient(
            verify=str(CA_FILE),
            timeout=timeout,
        ) as client:
            response = await client.request(
                method,
                f"{BACKEND_URL}{path}",
                json=json_body,
                headers={
                    "Accept": "application/json",
                },
            )
    except httpx.HTTPError as exc:
        raise HTTPException(
            status_code=503,
            detail=f"C backend is unavailable: {exc}",
        ) from exc

    try:
        content = response.json()
    except ValueError:
        content = {
            "status": "error",
            "message": response.text,
        }

    return response.status_code, content


@app.get(
    "/",
    include_in_schema=False,
)
async def root() -> RedirectResponse:
    return RedirectResponse(
        url="/docs",
        status_code=307,
    )


@app.get(
    "/api/v1/persons",
    response_model=PersonsResponse,
    tags=["Monitoring"],
    summary="Get current detected persons",
    description=(
        "Returns the current number of persons and timestamp from the C "
        "image-processing backend."
    ),
)
async def get_persons() -> JSONResponse:
    status_code, content = await request_backend_json(
        "GET",
        "/api/v1/persons",
    )

    return JSONResponse(
        status_code=status_code,
        content=content,
    )


@app.get(
    "/api/v1/telemetry",
    response_model=TelemetryResponse,
    tags=["Monitoring"],
    summary="Get live system telemetry",
    description=(
        "The C backend reads CPU temperature from /sys/class/thermal, "
        "available memory from /proc/meminfo and CPU usage from /proc/stat."
    ),
)
async def get_telemetry() -> JSONResponse:
    status_code, content = await request_backend_json(
        "GET",
        "/api/v1/telemetry",
    )

    return JSONResponse(
        status_code=status_code,
        content=content,
    )


@app.get(
    "/api/v1/history",
    response_model=HistoryResponse,
    tags=["Monitoring"],
    summary="Get the last five detection records",
    description=(
        "Returns up to five latest person-detection records maintained by "
        "the C backend."
    ),
)
async def get_history() -> JSONResponse:
    status_code, content = await request_backend_json(
        "GET",
        "/api/v1/history",
    )

    return JSONResponse(
        status_code=status_code,
        content=content,
    )


@app.post(
    "/api/v1/command",
    response_model=CommandAcceptedResponse,
    status_code=202,
    tags=["Command"],
    summary="Send a system command",
    description=(
        "Forwards the JSON request directly to the C backend. "
        "Sending cmd=reboot schedules a real system reboot."
    ),
    responses={
        400: {
            "description": "Invalid JSON request",
        },
        422: {
            "description": "Unsupported command",
        },
        503: {
            "description": "C backend unavailable",
        },
    },
)
async def send_command(
    command: CommandRequest,
) -> JSONResponse:
    status_code, content = await request_backend_json(
        "POST",
        "/api/v1/command",
        command.model_dump(),
    )

    return JSONResponse(
        status_code=status_code,
        content=content,
    )


@app.get(
    "/api/v1/stream",
    tags=["Monitoring"],
    summary="Get live MJPEG stream",
    description=(
        "Proxies the real MJPEG stream produced by the C backend. "
        "The duration parameter limits Swagger execution time so the "
        "test can finish."
    ),
    responses={
        200: {
            "description": "Live MJPEG stream",
            "content": {
                "multipart/x-mixed-replace": {},
            },
        },
        503: {
            "description": "C backend unavailable",
        },
    },
)
async def get_stream(
    duration: int = Query(
        default=3,
        ge=1,
        le=10,
        description="Number of seconds streamed during this request",
    ),
) -> Response:
    client = httpx.AsyncClient(
        verify=str(CA_FILE),
        timeout=httpx.Timeout(
            connect=5.0,
            read=None,
            write=5.0,
            pool=5.0,
        ),
    )

    try:
        request = client.build_request(
            "GET",
            f"{BACKEND_URL}/api/v1/stream",
        )

        response = await client.send(
            request,
            stream=True,
        )
    except httpx.HTTPError as exc:
        await client.aclose()

        raise HTTPException(
            status_code=503,
            detail=f"C stream backend is unavailable: {exc}",
        ) from exc

    if response.status_code != 200:
        body = await response.aread()
        media_type = response.headers.get(
            "content-type",
            "application/octet-stream",
        )

        await response.aclose()
        await client.aclose()

        return Response(
            content=body,
            status_code=response.status_code,
            media_type=media_type,
        )

    media_type = response.headers.get(
        "content-type",
        "multipart/x-mixed-replace; boundary=frame",
    )

    async def stream_generator():
        deadline = time.monotonic() + duration

        try:
            async for chunk in response.aiter_raw():
                if chunk:
                    yield chunk

                if time.monotonic() >= deadline:
                    break
        finally:
            await response.aclose()
            await client.aclose()

    return StreamingResponse(
        stream_generator(),
        status_code=200,
        media_type=media_type,
        headers={
            "Cache-Control": "no-store, no-cache, must-revalidate",
            "Pragma": "no-cache",
        },
    )
