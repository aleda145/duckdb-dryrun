#!/usr/bin/env python3
"""Print dryrun bytes vs DuckDB profiled bytes for representative queries."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlparse


ROOT = Path(__file__).resolve().parents[1]
DUCKDB = ROOT / "build" / "release" / "duckdb"
DRYRUN_EXTENSION = ROOT / "build" / "release" / "extension" / "dryrun" / "dryrun.duckdb_extension"
EXTENSION_DIRECTORY = Path("/tmp/duckdb-dryrun-extensions")
REMOTE_TITANIC = "https://assets.kavla.dev/demo/titanic.parquet"
REMOTE_PROPERTIES = "https://data.bostadsbussen.se/properties.parquet"


@dataclass(frozen=True)
class BenchmarkCase:
    name: str
    url: str
    query: str


def sql_string(value: str | Path) -> str:
    return str(value).replace("'", "''")


def run_duckdb(sql: str, db: Path, *, json_output: bool = False, quiet: bool = False) -> str:
    if not DUCKDB.exists():
        raise RuntimeError(f"DuckDB binary not found at {DUCKDB}; run `make` first")
    cmd = [str(DUCKDB)]
    if json_output:
        cmd.append("-json")
    cmd.extend([str(db), "-c", sql])
    result = subprocess.run(
        cmd,
        cwd=ROOT,
        text=True,
        stdout=subprocess.DEVNULL if quiet else subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"duckdb failed with exit code {result.returncode}\nSQL:\n{sql}\nSTDERR:\n{result.stderr}")
    return "" if quiet else result.stdout


def query_json(sql: str, db: Path) -> list[dict[str, Any]]:
    output = run_duckdb(sql, db, json_output=True)
    parsed = json.loads(output)
    if not isinstance(parsed, list):
        raise RuntimeError(f"expected DuckDB JSON result list, got {type(parsed).__name__}")
    return parsed


def dryrun(query: str, db: Path) -> dict[str, Any]:
    rows = query_json(
        f"{remote_prelude()}"
        f"LOAD '{sql_string(DRYRUN_EXTENSION)}'; "
        f"SELECT * FROM dryrun('{sql_string(query)}');",
        db,
    )
    if len(rows) != 1:
        raise RuntimeError(f"dryrun should return exactly one row, got {len(rows)}")
    return rows[0]


def remote_prelude() -> str:
    return (
        f"SET extension_directory='{sql_string(EXTENSION_DIRECTORY)}'; "
        "LOAD httpfs; "
    )


def setup_remote_extensions(db: Path) -> None:
    run_duckdb(
        f"SET extension_directory='{sql_string(EXTENSION_DIRECTORY)}'; INSTALL httpfs;",
        db,
        quiet=True,
    )


def profile(case: BenchmarkCase, db: Path, temp_dir: Path) -> dict[str, Any]:
    profile_path = temp_dir / f"profile_{case.name}.json"
    if profile_path.exists():
        profile_path.unlink()
    table_name = "benchmark_" + "".join(ch if ch.isalnum() else "_" for ch in case.name)
    run_duckdb(
        f"""
        {remote_prelude()}
        PRAGMA enable_profiling='json';
        PRAGMA profiling_output='{sql_string(profile_path)}';
        CREATE OR REPLACE TEMP TABLE {table_name} AS {case.query};
        PRAGMA disable_profiling;
        """,
        db,
        quiet=True,
    )
    if not profile_path.exists():
        raise RuntimeError(f"profiling output was not created at {profile_path}")
    return json.loads(profile_path.read_text())


def format_ratio(numerator: int | float, denominator: int | float) -> str:
    if denominator == 0:
        return "n/a"
    return f"{numerator / denominator:.3f}x"


def print_table(headers: list[str], rows: list[list[Any]]) -> None:
    text_rows = [[str(value) for value in row] for row in rows]
    widths = [max(len(headers[index]), *(len(row[index]) for row in text_rows)) for index in range(len(headers))]
    print(" | ".join(headers[index].ljust(widths[index]) for index in range(len(headers))))
    print("-+-".join("-" * width for width in widths))
    for row in text_rows:
        print(" | ".join(row[index].ljust(widths[index]) for index in range(len(headers))))


def default_cases() -> list[BenchmarkCase]:
    return [
        BenchmarkCase("titanic_full", REMOTE_TITANIC, f"SELECT * FROM '{sql_string(REMOTE_TITANIC)}'"),
        BenchmarkCase(
            "titanic_PassengerId",
            REMOTE_TITANIC,
            f"SELECT PassengerId FROM '{sql_string(REMOTE_TITANIC)}'",
        ),
        BenchmarkCase("properties_full", REMOTE_PROPERTIES, f"SELECT * FROM '{sql_string(REMOTE_PROPERTIES)}'"),
        BenchmarkCase(
            "properties_price",
            REMOTE_PROPERTIES,
            f"SELECT price FROM '{sql_string(REMOTE_PROPERTIES)}'",
        ),
    ]


def name_from_url(url: str, fallback: str) -> str:
    filename = Path(unquote(urlparse(url).path)).name
    if not filename:
        return fallback
    if filename.lower().endswith(".parquet"):
        filename = filename[: -len(".parquet")]
    return filename or fallback


def custom_cases(urls: list[str], column: str | None) -> list[BenchmarkCase]:
    cases = []
    for index, url in enumerate(urls, start=1):
        fallback = "custom" if len(urls) == 1 else f"custom_{index}"
        name = name_from_url(url, fallback)
        cases.append(BenchmarkCase(f"{name}_full", url, f"SELECT * FROM '{sql_string(url)}'"))
        if column is not None:
            cases.append(
                BenchmarkCase(
                    f"{name}_{column}",
                    url,
                    f"SELECT {column} FROM '{sql_string(url)}'",
                )
            )
    return cases


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Benchmark dryrun bytes against DuckDB profiled bytes for remote Parquet")
    parser.add_argument("urls", nargs="*", help="remote Parquet URL(s) to benchmark instead of the built-in defaults")
    parser.add_argument("--column", help="column to project for custom URL benchmarks")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    cases = custom_cases(args.urls, args.column) if args.urls else default_cases()
    temp_dir = Path(tempfile.mkdtemp(prefix="dryrun-benchmark-"))
    db = temp_dir / "benchmark.duckdb"
    try:
        setup_remote_extensions(db)
        rows = []
        for case in cases:
            estimate = dryrun(case.query, db)
            profile_data = profile(case, db, temp_dir)
            dryrun_bytes = int(estimate["estimated_compressed_bytes"])
            profiled_bytes = int(profile_data["total_bytes_read"])
            rows.append(
                [
                    case.name,
                    dryrun_bytes,
                    profiled_bytes,
                    format_ratio(profiled_bytes, dryrun_bytes),
                    estimate["confidence"],
                ]
            )

        print("remote_sources:")
        seen_urls = []
        for case in cases:
            if case.url not in seen_urls:
                seen_urls.append(case.url)
                print(f"- {case.url}")
        print()
        print_table(
            ["case", "dryrun_bytes", "profiled_bytes", "profiled/dryrun", "confidence"],
            rows,
        )
        return 0
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"benchmark failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
