#!/usr/bin/env python3
"""
Sample code file for benchmarking text rendering.
This file contains various Python constructs to test syntax highlighting.
"""

import os
import sys
from typing import List, Dict, Optional, Union, Callable
from dataclasses import dataclass
from collections import defaultdict
import asyncio
import json


@dataclass
class BenchmarkResult:
    """Result from a single benchmark run."""

    name: str
    iterations: int
    total_time_ms: float
    gc_count: int
    gc_time_ms: float

    @property
    def avg_time_ms(self) -> float:
        """Calculate average time per iteration."""
        return self.total_time_ms / self.iterations if self.iterations > 0 else 0.0

    def to_dict(self) -> Dict:
        """Convert to dictionary for JSON serialization."""
        return {
            "name": self.name,
            "iterations": self.iterations,
            "total_time_ms": self.total_time_ms,
            "avg_time_ms": self.avg_time_ms,
            "gc_count": self.gc_count,
            "gc_time_ms": self.gc_time_ms,
        }


class BenchmarkRunner:
    """Runs and collects benchmark results."""

    def __init__(self, warmup_iterations: int = 3, measure_iterations: int = 10):
        self.warmup_iterations = warmup_iterations
        self.measure_iterations = measure_iterations
        self.results: List[BenchmarkResult] = []
        self._callbacks: List[Callable] = []

    def register_callback(self, callback: Callable[[BenchmarkResult], None]) -> None:
        """Register a callback to be called after each benchmark."""
        self._callbacks.append(callback)

    def run_benchmark(
        self, name: str, func: Callable[[], None], iterations: Optional[int] = None
    ) -> BenchmarkResult:
        """
        Run a single benchmark with warmup.

        Args:
            name: Name of the benchmark
            func: Function to benchmark
            iterations: Override default iterations

        Returns:
            BenchmarkResult with timing data
        """
        iters = iterations or self.measure_iterations

        # Warmup phase
        for _ in range(self.warmup_iterations):
            func()

        # Measurement phase
        import time
        import gc

        gc.collect()
        gc_count_before = gc.get_count()[0]

        start = time.perf_counter()
        for _ in range(iters):
            func()
        end = time.perf_counter()

        gc_count_after = gc.get_count()[0]

        result = BenchmarkResult(
            name=name,
            iterations=iters,
            total_time_ms=(end - start) * 1000,
            gc_count=gc_count_after - gc_count_before,
            gc_time_ms=0.0,  # Python doesn't easily expose GC time
        )

        self.results.append(result)

        for callback in self._callbacks:
            callback(result)

        return result

    def to_csv(self) -> str:
        """Export results to CSV format."""
        lines = ["name,iterations,total_ms,avg_ms,gc_count,gc_time_ms"]
        for r in self.results:
            lines.append(
                f"{r.name},{r.iterations},{r.total_time_ms:.3f},"
                f"{r.avg_time_ms:.3f},{r.gc_count},{r.gc_time_ms:.3f}"
            )
        return "\n".join(lines)


async def async_benchmark_example():
    """Example of async code for testing syntax highlighting."""
    results = defaultdict(list)

    async def fetch_data(url: str) -> Dict:
        # Simulated async fetch
        await asyncio.sleep(0.01)
        return {"url": url, "status": "ok"}

    urls = [f"https://example.com/api/{i}" for i in range(10)]

    tasks = [fetch_data(url) for url in urls]
    responses = await asyncio.gather(*tasks)

    for resp in responses:
        results[resp["status"]].append(resp["url"])

    return dict(results)


def generate_test_data(
    lines: int = 1000, chars_per_line: int = 80, include_unicode: bool = True
) -> str:
    """
    Generate test data for rendering benchmarks.

    Includes various character types:
    - ASCII alphanumeric
    - Punctuation
    - Unicode (if enabled)
    """
    import random
    import string

    ascii_chars = string.ascii_letters + string.digits + string.punctuation + " "
    unicode_samples = "Hello World Test Sample"

    result_lines = []
    for i in range(lines):
        if include_unicode and i % 10 == 0:
            # Every 10th line includes Unicode
            line = f"Line {i}: " + "".join(
                random.choice(unicode_samples) for _ in range(chars_per_line - 10)
            )
        else:
            line = f"Line {i}: " + "".join(
                random.choice(ascii_chars) for _ in range(chars_per_line - 10)
            )
        result_lines.append(line[:chars_per_line])

    return "\n".join(result_lines)


# Long dictionary for testing rendering of data structures
SAMPLE_CONFIG = {
    "rendering": {
        "backend": "skia",
        "gpu_enabled": True,
        "antialiasing": "subpixel",
        "font_hinting": "slight",
        "dpi": 96,
    },
    "benchmarks": {
        "text_rendering": {
            "enabled": True,
            "lines": 10000,
            "include_unicode": True,
        },
        "scrolling": {
            "enabled": True,
            "iterations": 500,
            "modes": ["line", "page", "random"],
        },
        "images": {
            "enabled": True,
            "formats": ["png", "jpeg", "xpm"],
            "sizes": [32, 64, 128, 256],
        },
    },
    "output": {
        "format": "csv",
        "include_system_info": True,
        "timestamps": True,
    },
}


def main():
    """Main entry point for the benchmark script."""
    runner = BenchmarkRunner(warmup_iterations=3, measure_iterations=10)

    # Register a simple logging callback
    runner.register_callback(
        lambda r: print(f"  {r.name}: {r.avg_time_ms:.2f} ms/iter")
    )

    print("Running benchmarks...")
    print()

    # Example benchmark: string concatenation
    def string_concat():
        s = ""
        for i in range(1000):
            s += str(i)

    runner.run_benchmark("string_concat", string_concat)

    # Example benchmark: list operations
    def list_ops():
        lst = []
        for i in range(10000):
            lst.append(i)
        lst.sort(reverse=True)
        return sum(lst)

    runner.run_benchmark("list_operations", list_ops)

    # Example benchmark: dict operations
    def dict_ops():
        d = {}
        for i in range(10000):
            d[f"key_{i}"] = i * 2
        return sum(d.values())

    runner.run_benchmark("dict_operations", dict_ops)

    print()
    print("Results:")
    print(runner.to_csv())

    return 0


if __name__ == "__main__":
    sys.exit(main())
