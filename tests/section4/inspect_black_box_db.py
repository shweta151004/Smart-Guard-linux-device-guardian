#!/usr/bin/env python3

import csv
import json
import sqlite3
import sys
from datetime import datetime
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parents[2]
OUTPUT_DIR = (
    PROJECT_DIR
    / "docs/tests/section4/test_4-2_black_box"
)

FINAL_FILE = OUTPUT_DIR / "black_box_database_evidence.txt"
CSV_FILE = OUTPUT_DIR / "black_box_records.csv"

TABLE_KEYWORDS = (
    "event",
    "events",
    "log",
    "logs",
    "black",
    "alarm",
    "guard",
    "detection",
    "history",
)

COLUMN_KEYWORDS = (
    "event",
    "type",
    "timestamp",
    "time",
    "created",
    "student",
    "person",
    "guard",
    "alarm",
    "temperature",
)


def quote_identifier(value):
    return '"' + value.replace('"', '""') + '"'


def open_read_only(path):
    uri = f"file:{path.resolve()}?mode=ro"

    connection = sqlite3.connect(
        uri,
        uri=True,
    )

    connection.row_factory = sqlite3.Row

    return connection


def get_table_names(connection):
    query = """
        SELECT name
        FROM sqlite_master
        WHERE type = 'table'
          AND name NOT LIKE 'sqlite_%'
        ORDER BY name
    """

    return [
        row[0]
        for row in connection.execute(query)
    ]


def get_table_columns(connection, table):
    query = (
        "PRAGMA table_info("
        + quote_identifier(table)
        + ")"
    )

    return [
        row[1]
        for row in connection.execute(query)
    ]


def get_table_count(connection, table):
    query = (
        "SELECT COUNT(*) FROM "
        + quote_identifier(table)
    )

    return int(
        connection.execute(query).fetchone()[0]
    )


def calculate_table_score(table, columns):
    score = 0
    table_lower = table.lower()

    for keyword in TABLE_KEYWORDS:
        if keyword in table_lower:
            score += 10

    for column in columns:
        column_lower = column.lower()

        for keyword in COLUMN_KEYWORDS:
            if keyword in column_lower:
                score += 2

    return score


def select_order_column(columns):
    preferred_columns = (
        "id",
        "event_id",
        "created_at",
        "timestamp",
        "event_time",
        "time",
        "datetime",
    )

    lower_map = {
        column.lower(): column
        for column in columns
    }

    for preferred in preferred_columns:
        if preferred in lower_map:
            return lower_map[preferred]

    return None


def fetch_recent_rows(
    connection,
    table,
    columns,
    limit=500,
):
    order_column = select_order_column(columns)

    query = (
        "SELECT * FROM "
        + quote_identifier(table)
    )

    if order_column is not None:
        query += (
            " ORDER BY "
            + quote_identifier(order_column)
            + " DESC"
        )
    else:
        query += " ORDER BY rowid DESC"

    query += f" LIMIT {int(limit)}"

    try:
        return [
            dict(row)
            for row in connection.execute(query)
        ]
    except sqlite3.OperationalError:
        fallback_query = (
            "SELECT * FROM "
            + quote_identifier(table)
            + f" LIMIT {int(limit)}"
        )

        return [
            dict(row)
            for row in connection.execute(
                fallback_query
            )
        ]


def row_as_text(row):
    return json.dumps(
        row,
        ensure_ascii=False,
        default=str,
    ).lower()


OUTPUT_DIR.mkdir(
    parents=True,
    exist_ok=True,
)

database_paths = []

for path in PROJECT_DIR.rglob("*"):
    if not path.is_file():
        continue

    try:
        with path.open("rb") as handle:
            header = handle.read(16)
    except OSError:
        continue

    if header == b"SQLite format 3\x00":
        database_paths.append(path)

if not database_paths:
    print(
        "ERROR: No SQLite database was found "
        "inside the project."
    )
    sys.exit(1)

database_candidates = []

for database_path in database_paths:
    try:
        connection = open_read_only(
            database_path
        )

        integrity_result = connection.execute(
            "PRAGMA integrity_check"
        ).fetchone()[0]

        tables = get_table_names(
            connection
        )

        table_information = []

        for table in tables:
            columns = get_table_columns(
                connection,
                table,
            )

            row_count = get_table_count(
                connection,
                table,
            )

            table_score = calculate_table_score(
                table,
                columns,
            )

            table_information.append({
                "name": table,
                "columns": columns,
                "count": row_count,
                "score": table_score,
            })

        database_score = sum(
            item["score"]
            for item in table_information
        )

        if "data" in str(database_path).lower():
            database_score += 5

        if "docs/tests" in str(database_path):
            database_score -= 20

        database_candidates.append({
            "path": database_path,
            "integrity": integrity_result,
            "tables": table_information,
            "score": database_score,
        })

        connection.close()

    except sqlite3.Error:
        continue

if not database_candidates:
    print(
        "ERROR: SQLite files were found, "
        "but none could be opened."
    )
    sys.exit(1)

database_candidates.sort(
    key=lambda item: (
        item["score"],
        item["path"].stat().st_mtime,
    ),
    reverse=True,
)

selected_database = database_candidates[0]
database_path = selected_database["path"]

connection = open_read_only(
    database_path
)

matching_records = []
recent_event_records = []

for table_info in selected_database["tables"]:
    table_name = table_info["name"]
    columns = table_info["columns"]

    rows = fetch_recent_rows(
        connection,
        table_name,
        columns,
    )

    for row in rows:
        text = row_as_text(row)
        match_score = 0

        if "guard_human_detected" in text:
            match_score += 100

        if "device-001" in text:
            match_score += 20

        if "guard" in text:
            match_score += 10

        if "alarm" in text:
            match_score += 10

        if "person" in text:
            match_score += 5

        if match_score > 0:
            matching_records.append({
                "source_table": table_name,
                "match_score": match_score,
                **row,
            })

    if table_info["score"] > 0:
        for row in rows[:20]:
            recent_event_records.append({
                "source_table": table_name,
                **row,
            })

matching_records.sort(
    key=lambda row: row["match_score"],
    reverse=True,
)

if matching_records:
    displayed_records = matching_records[:20]
else:
    displayed_records = recent_event_records[:20]

guard_event_found = any(
    "guard_human_detected" in row_as_text(row)
    for row in matching_records
)

student_id_found = any(
    "device-001" in row_as_text(row)
    for row in matching_records
)

with FINAL_FILE.open(
    "w",
    encoding="utf-8",
) as output:
    def write(line=""):
        print(line)
        output.write(line + "\n")

    write(
        "========== TEST 4-2: BLACK BOX DATABASE =========="
    )

    write(
        "Inspection time: "
        + datetime.now().astimezone().isoformat()
    )

    write()
    write(
        "========== SELECTED SQLITE DATABASE =========="
    )

    write(
        f"Database path: {database_path}"
    )

    write(
        "Database size: "
        f"{database_path.stat().st_size} bytes"
    )

    write(
        "Integrity check: "
        f"{selected_database['integrity']}"
    )

    write()
    write(
        "========== DATABASE TABLES =========="
    )

    for table_info in selected_database["tables"]:
        write(
            f"Table: {table_info['name']}"
        )

        write(
            f"Rows: {table_info['count']}"
        )

        write(
            "Columns: "
            + ", ".join(
                table_info["columns"]
            )
        )

        write()

    write(
        "========== STORED BLACK-BOX RECORDS =========="
    )

    if displayed_records:
        for index, record in enumerate(
            displayed_records,
            start=1,
        ):
            write(
                f"Record {index}: "
                + json.dumps(
                    record,
                    ensure_ascii=False,
                    default=str,
                )
            )
    else:
        write(
            "No stored black-box records were found."
        )

    write()
    write(
        "========== TEST RESULT =========="
    )

    if selected_database["integrity"] == "ok":
        write(
            "PASS: SQLite integrity check returned ok."
        )
    else:
        write(
            "FAIL: SQLite integrity check failed."
        )

    if selected_database["tables"]:
        write(
            "PASS: Database tables were discovered."
        )
    else:
        write(
            "FAIL: No database table was discovered."
        )

    if displayed_records:
        write(
            "PASS: Stored black-box records were retrieved."
        )
    else:
        write(
            "FAIL: No stored black-box record was retrieved."
        )

    if guard_event_found:
        write(
            "PASS: A stored guard_human_detected event was found."
        )
    else:
        write(
            "WARNING: No guard_human_detected event was found."
        )

    if student_id_found:
        write(
            "PASS: Student ID device-001 was found "
            "in stored records."
        )
    else:
        write(
            "WARNING: Student ID was not found "
            "in matching records."
        )

if displayed_records:
    fieldnames = []

    for record in displayed_records:
        for key in record:
            if key not in fieldnames:
                fieldnames.append(key)

    with CSV_FILE.open(
        "w",
        newline="",
        encoding="utf-8",
    ) as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=fieldnames,
            extrasaction="ignore",
        )

        writer.writeheader()
        writer.writerows(displayed_records)

connection.close()

print()
print(f"Final evidence: {FINAL_FILE}")

if displayed_records:
    print(f"CSV: {CSV_FILE}")

if (
    selected_database["integrity"] != "ok"
    or not selected_database["tables"]
    or not displayed_records
):
    sys.exit(1)
