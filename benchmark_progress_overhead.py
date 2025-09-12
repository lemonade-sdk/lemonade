#!/usr/bin/env python3
"""
Performance benchmark to measure overhead of prefill progress tracking.
This script compares streaming performance with progress updates vs without.

Usage:
    python benchmark_progress_overhead.py

Requirements:
    - Lemonade server must be running on port 8000
    - Qwen 0.6B model should be available (or modify MODEL_NAME below)
"""

import time
import json
import statistics
from typing import List, Dict, Any, Tuple
from dataclasses import dataclass
from openai import OpenAI
import argparse
import sys

# Configuration
MODEL_NAME = "Qwen3-0.6B-GGUF"  # Small model for testing overhead
BASE_URL = "http://localhost:8000/api/v1"
API_KEY = "lemonade"

# Test prompts of varying lengths
TEST_PROMPTS = {
    "short": "What is 2+2?",
    "medium": "Explain the concept of artificial intelligence " * 10,  # ~100 tokens
    "long": "Please provide a detailed analysis of the following topic: " + "artificial intelligence and machine learning " * 50,  # ~500+ tokens
    "very_long": "Analyze this comprehensive text about technology: " + "The rapid advancement of artificial intelligence, machine learning, deep learning, neural networks, and computational frameworks " * 100,  # ~1500+ tokens
}


@dataclass
class BenchmarkResult:
    """Store results from a single benchmark run."""
    prompt_length: str
    time_to_first_token: float
    total_time: float
    tokens_generated: int
    tokens_per_second: float
    has_progress_updates: bool
    progress_count: int


def run_single_benchmark(
    client: OpenAI,
    prompt: str,
    prompt_length: str,
    max_tokens: int = 50
) -> BenchmarkResult:
    """Run a single benchmark test and collect metrics."""
    
    start_time = time.time()
    first_token_time = None
    tokens_generated = 0
    has_progress_updates = False
    progress_count = 0
    
    try:
        stream = client.chat.completions.create(
            model=MODEL_NAME,
            messages=[{"role": "user", "content": prompt}],
            stream=True,
            max_tokens=max_tokens,
        )
        
        for chunk in stream:
            if chunk.choices and len(chunk.choices) > 0:
                delta = chunk.choices[0].delta
                
                # Check for progress updates (tool calls)
                if hasattr(delta, 'tool_calls') and delta.tool_calls:
                    has_progress_updates = True
                    progress_count += 1
                
                # Check for actual content
                if hasattr(delta, 'content') and delta.content:
                    if first_token_time is None:
                        first_token_time = time.time()
                    tokens_generated += 1
        
        total_time = time.time() - start_time
        time_to_first_token = (first_token_time - start_time) if first_token_time else total_time
        tokens_per_second = tokens_generated / total_time if total_time > 0 else 0
        
        return BenchmarkResult(
            prompt_length=prompt_length,
            time_to_first_token=time_to_first_token,
            total_time=total_time,
            tokens_generated=tokens_generated,
            tokens_per_second=tokens_per_second,
            has_progress_updates=has_progress_updates,
            progress_count=progress_count,
        )
        
    except Exception as e:
        print(f"Error during benchmark: {e}")
        return None


def run_benchmarks(num_runs: int = 5) -> Dict[str, List[BenchmarkResult]]:
    """Run multiple benchmark iterations for each prompt length."""
    
    client = OpenAI(base_url=BASE_URL, api_key=API_KEY)
    results = {length: [] for length in TEST_PROMPTS.keys()}
    
    print(f"Running {num_runs} iterations for each prompt length...")
    print("-" * 60)
    
    for prompt_length, prompt in TEST_PROMPTS.items():
        print(f"\nTesting {prompt_length} prompt ({len(prompt.split())} words)...")
        
        for i in range(num_runs):
            print(f"  Run {i+1}/{num_runs}...", end="")
            result = run_single_benchmark(client, prompt, prompt_length)
            
            if result:
                results[prompt_length].append(result)
                print(f" TTFT: {result.time_to_first_token:.3f}s, "
                      f"TPS: {result.tokens_per_second:.1f}, "
                      f"Progress updates: {result.progress_count}")
            else:
                print(" FAILED")
            
            # Small delay between runs
            time.sleep(1)
    
    return results


def calculate_statistics(results: List[BenchmarkResult]) -> Dict[str, Any]:
    """Calculate statistics for a set of benchmark results."""
    
    if not results:
        return {}
    
    ttft_values = [r.time_to_first_token for r in results]
    tps_values = [r.tokens_per_second for r in results]
    progress_counts = [r.progress_count for r in results]
    
    return {
        "ttft_mean": statistics.mean(ttft_values),
        "ttft_median": statistics.median(ttft_values),
        "ttft_stdev": statistics.stdev(ttft_values) if len(ttft_values) > 1 else 0,
        "tps_mean": statistics.mean(tps_values),
        "tps_median": statistics.median(tps_values),
        "tps_stdev": statistics.stdev(tps_values) if len(tps_values) > 1 else 0,
        "progress_updates_mean": statistics.mean(progress_counts),
        "has_progress": any(r.has_progress_updates for r in results),
    }


def print_results_table(all_results: Dict[str, List[BenchmarkResult]]):
    """Print results in a formatted table."""
    
    print("\n" + "=" * 80)
    print("BENCHMARK RESULTS")
    print("=" * 80)
    
    # Header
    print(f"\n{'Prompt':<12} | {'TTFT (s)':<20} | {'TPS':<20} | {'Progress':<15}")
    print(f"{'Length':<12} | {'Mean ± StdDev':<20} | {'Mean ± StdDev':<20} | {'Updates (avg)':<15}")
    print("-" * 80)
    
    for prompt_length in ["short", "medium", "long", "very_long"]:
        if prompt_length not in all_results:
            continue
            
        results = all_results[prompt_length]
        if not results:
            continue
            
        stats = calculate_statistics(results)
        
        ttft_str = f"{stats['ttft_mean']:.3f} ± {stats['ttft_stdev']:.3f}"
        tps_str = f"{stats['tps_mean']:.1f} ± {stats['tps_stdev']:.1f}"
        progress_str = f"{stats['progress_updates_mean']:.1f}"
        
        print(f"{prompt_length:<12} | {ttft_str:<20} | {tps_str:<20} | {progress_str:<15}")
    
    print("-" * 80)
    
    # Summary statistics
    print("\nSUMMARY:")
    total_runs = sum(len(results) for results in all_results.values())
    runs_with_progress = sum(
        1 for results in all_results.values() 
        for r in results if r.has_progress_updates
    )
    
    print(f"  Total runs: {total_runs}")
    print(f"  Runs with progress updates: {runs_with_progress} ({100*runs_with_progress/total_runs:.1f}%)")
    
    # Calculate overall overhead (comparing short vs long prompts)
    if "short" in all_results and "very_long" in all_results:
        short_stats = calculate_statistics(all_results["short"])
        long_stats = calculate_statistics(all_results["very_long"])
        
        if short_stats and long_stats:
            ttft_diff = long_stats["ttft_mean"] - short_stats["ttft_mean"]
            print(f"\n  TTFT difference (very_long - short): {ttft_diff:.3f}s")
            print(f"  This represents the overhead of processing longer prompts")


def main():
    parser = argparse.ArgumentParser(
        description="Benchmark prefill progress tracking overhead"
    )
    parser.add_argument(
        "--runs",
        type=int,
        default=5,
        help="Number of benchmark runs per prompt length (default: 5)"
    )
    parser.add_argument(
        "--model",
        type=str,
        default=MODEL_NAME,
        help=f"Model to use for benchmarking (default: {MODEL_NAME})"
    )
    parser.add_argument(
        "--url",
        type=str,
        default=BASE_URL,
        help=f"Server URL (default: {BASE_URL})"
    )
    
    args = parser.parse_args()
    
    # Update globals if provided
    global MODEL_NAME, BASE_URL
    MODEL_NAME = args.model
    BASE_URL = args.url
    
    print(f"Lemonade Progress Tracking Performance Benchmark")
    print(f"================================================")
    print(f"Model: {MODEL_NAME}")
    print(f"Server: {BASE_URL}")
    print(f"Runs per prompt: {args.runs}")
    print()
    
    # Check server availability
    try:
        client = OpenAI(base_url=BASE_URL, api_key=API_KEY)
        # Quick test to ensure model is loaded
        print("Testing server connectivity...", end="")
        response = client.chat.completions.create(
            model=MODEL_NAME,
            messages=[{"role": "user", "content": "test"}],
            max_tokens=1,
        )
        print(" OK")
    except Exception as e:
        print(f"\nError: Cannot connect to server at {BASE_URL}")
        print(f"Details: {e}")
        print("\nPlease ensure:")
        print(f"  1. Lemonade server is running on {BASE_URL}")
        print(f"  2. Model {MODEL_NAME} is loaded")
        sys.exit(1)
    
    # Run benchmarks
    results = run_benchmarks(num_runs=args.runs)
    
    # Display results
    print_results_table(results)
    
    print("\nBenchmark complete!")
    print("\nNOTE: To compare with/without progress tracking:")
    print("  1. Run this benchmark on the main branch (baseline)")
    print("  2. Run this benchmark on the PR branch (with progress)")
    print("  3. Compare the TTFT and TPS values between runs")


if __name__ == "__main__":
    main()