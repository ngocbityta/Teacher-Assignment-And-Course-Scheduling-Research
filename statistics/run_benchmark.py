#!/usr/bin/env python3
"""
Benchmark Script for Algorithm Comparison
Compares Heuristic (Phase 2 + Phase 3) vs CP-SAT solver on test cases.
Outputs results to CSV for chart generation.
"""

import requests
import json
import csv
import sys
import time
from datetime import datetime

# Configuration
BASE_URL = "http://localhost:8081"
HEURISTIC_ENDPOINT = "/testcase/run"
CPSAT_ENDPOINT = "/testcase/run-cpsat"
OUTPUT_FILE = "statistics/benchmark_results.csv"
NUM_TESTCASES = 10
HEURISTIC_TIME_LIMIT = 30  # seconds
CPSAT_TIME_LIMIT = 30     # seconds

def run_benchmark(test_case_name, endpoint, time_limit):
    """Run a single benchmark test case."""
    url = f"{BASE_URL}{endpoint}"
    payload = {
        "test_case_name": test_case_name,
        "time_limit_seconds": time_limit
    }
    
    try:
        response = requests.post(url, json=payload, timeout=time_limit + 10)
        if response.status_code == 200:
            return response.json()
        else:
            print(f"  Error: HTTP {response.status_code}")
            return None
    except requests.exceptions.Timeout:
        print(f"  Timeout after {time_limit + 10}s")
        return None
    except requests.exceptions.ConnectionError:
        print(f"  Connection error - is the server running?")
        return None

def main():
    print("=" * 60)
    print("Algorithm Comparison Benchmark")
    print(f"Started at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("=" * 60)
    
    results = []
    
    test_cases = []
    if len(sys.argv) > 1:
        test_cases = sys.argv[1:]
    else:
        test_cases = [f"test_case_{i:02d}.json" for i in range(1, NUM_TESTCASES + 1)]

    for i, test_case_name in enumerate(test_cases, 1):
        print(f"\n[{i}/{len(test_cases)}] Running {test_case_name}...")
        
        # Run Heuristic
        print(f"  Heuristic (limit: {HEURISTIC_TIME_LIMIT}s)...", end=" ", flush=True)
        heuristic_result = run_benchmark(test_case_name, HEURISTIC_ENDPOINT, HEURISTIC_TIME_LIMIT)
        if heuristic_result:
            print(f"Done ({heuristic_result.get('timing', {}).get('total_time_ms', 0)}ms)")
        else:
            print("Failed")
            continue
        
        # Run CP-SAT
        print(f"  CP-SAT    (limit: {CPSAT_TIME_LIMIT}s)...", end=" ", flush=True)
        cpsat_result = run_benchmark(test_case_name, CPSAT_ENDPOINT, CPSAT_TIME_LIMIT)
        if cpsat_result:
            print(f"Done ({cpsat_result.get('timing', {}).get('total_time_ms', 0)}ms)")
        else:
            print("Failed (setting to zeros)")
            cpsat_result = {}

        # Extract statistics
        h_stats = heuristic_result.get("statistics", {})
        h_timing = heuristic_result.get("timing", {})
        h_solution = heuristic_result.get("solution", {})
        
        c_timing = cpsat_result.get("timing", {})
        c_solution = cpsat_result.get("solution", {})
        
        h_obj = h_solution.get("objective_value", 0)
        c_obj = c_solution.get("objective_value", 0)
        
        row = {
            "test_case": test_case_name,
            "num_teachers": h_stats.get("num_teachers", 0),
            "num_classrooms": h_stats.get("num_classrooms", 0),
            "num_courses": h_stats.get("num_courses", 0),
            "num_sections": h_stats.get("num_sections", 0),
            "num_days": h_stats.get("num_days", 0),
            "num_periods": h_stats.get("num_periods", 0),
            # Heuristic timing
            "heuristic_total_ms": h_timing.get("total_time_ms", 0),
            "heuristic_objective": h_obj,
            "heuristic_violations": h_solution.get("hard_constraint_violations", 0),
            # CP-SAT timing
            "cpsat_total_ms": c_timing.get("total_time_ms", 0),
            "cpsat_objective": c_obj,
            "cpsat_violations": c_solution.get("hard_constraint_violations", 0),
            # Accuracy
            "delta": c_obj - h_obj if c_obj > 0 else 0
        }
        
        results.append(row)
        print(f"  Objective: Heuristic={h_obj:.2f}, CP-SAT={c_obj:.2f} (Delta: {row['delta']:.2f})")
    
    # Write to CSV
    if results:
        fieldnames = list(results[0].keys())
        with open(OUTPUT_FILE, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(results)
        print(f"\n{'=' * 60}")
        print(f"Results saved to: {OUTPUT_FILE}")
        print(f"Total test cases: {len(results)}")
    else:
        print("\nNo results to save.")
    
    print(f"\nFinished at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("=" * 60)

if __name__ == "__main__":
    main()
