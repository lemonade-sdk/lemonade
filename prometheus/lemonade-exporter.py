#!/usr/bin/env python3
"""
Lemonade Server Prometheus Exporter

Scrapes metrics from Lemonade Server's /api/v1/stats and /api/v1/health
endpoints and exposes them in Prometheus format using the official
prometheus_client library.

Usage:
    python3 lemonade-exporter.py [--lemonade-url URL] [--port PORT]

Examples:
    python3 lemonade-exporter.py
    python3 lemonade-exporter.py --lemonade-url http://localhost:8000 --port 9091
"""

import argparse
import json
import subprocess
import sys
import time
from threading import Lock
from typing import Dict, Optional

import requests
from prometheus_client import (
    Counter,
    Gauge,
    Histogram,
    start_http_server,
    REGISTRY,
)

class LemonadeExporter:
    """Exports Lemonade Server metrics in Prometheus format."""

    def __init__(self, lemonade_url: str = "http://localhost:8000"):
        """
        Initialize exporter.

        Args:
            lemonade_url: Base URL of Lemonade Server
        """
        self.lemonade_url = lemonade_url.rstrip("/")
        self.stats_url = f"{self.lemonade_url}/api/v1/stats"
        self.health_url = f"{self.lemonade_url}/api/v1/health"
        self.metrics_lock = Lock()
        self.last_update = 0
        self.update_interval = 5.0  # Update every 5 seconds
        
        # Track cumulative totals for counters
        self.cumulative_input_tokens = 0
        self.cumulative_output_tokens = 0
        self.cumulative_prompt_tokens = 0
        
        # Track last processed stats to avoid double-counting decode times
        self.last_stats_hash = None
        
        # Track requests for concurrent users/sessions estimation
        self.request_timestamps = []  # List of (timestamp, duration) tuples
        self.total_requests = 0
        self.last_request_time = 0
        
        # Initialize Prometheus metrics
        self.server_up = Gauge(
            "lemonade_server_up",
            "Whether Lemonade Server is up",
        )
        
        self.tokens_per_second = Gauge(
            "lemonade_tokens_per_second",
            "Tokens generated per second",
        )
        
        self.time_to_first_token = Gauge(
            "lemonade_time_to_first_token_seconds",
            "Time to first token in seconds",
        )
        
        # Note: Lemonade's /stats endpoint returns per-request values, not cumulative
        # We expose both: gauges for last request values and counters for cumulative totals
        self.input_tokens_last = Gauge(
            "lemonade_input_tokens_last",
            "Number of input tokens from last request",
        )
        
        self.output_tokens_last = Gauge(
            "lemonade_output_tokens_last",
            "Number of output tokens from last request",
        )
        
        self.prompt_tokens_last = Gauge(
            "lemonade_prompt_tokens_last",
            "Number of prompt tokens from last request",
        )
        
        # Cache metrics (calculated from prompt_tokens and input_tokens)
        self.cached_tokens_last = Gauge(
            "lemonade_cached_tokens_last",
            "Number of cached tokens from last request (prompt_tokens - input_tokens)",
        )
        
        self.cache_hit_rate = Gauge(
            "lemonade_cache_hit_rate",
            "Cache hit rate for last request (cached_tokens / prompt_tokens, 0-1)",
        )
        
        # Cumulative counters (we track these ourselves)
        self.input_tokens_total = Counter(
            "lemonade_input_tokens_total",
            "Total cumulative input tokens processed",
        )
        
        self.output_tokens_total = Counter(
            "lemonade_output_tokens_total",
            "Total cumulative output tokens generated",
        )
        
        self.prompt_tokens_total = Counter(
            "lemonade_prompt_tokens_total",
            "Total cumulative prompt tokens",
        )
        
        self.models_loaded = Gauge(
            "lemonade_models_loaded",
            "Number of models currently loaded",
        )
        
        self.model_info = Gauge(
            "lemonade_model_info",
            "Information about loaded models",
            ["model_name", "type", "device"],
        )
        
        self.max_models = Gauge(
            "lemonade_max_models",
            "Maximum number of models that can be loaded",
            ["type"],
        )
        
        self.decode_token_time = Histogram(
            "lemonade_decode_token_time_seconds",
            "Time to decode each token",
            buckets=[0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0],
        )
        
        self.last_update_time = Gauge(
            "lemonade_exporter_last_update_timestamp_seconds",
            "Timestamp of last successful update",
        )
        
        # Concurrent users and sessions tracking
        self.concurrent_users = Gauge(
            "lemonade_concurrent_users",
            "Estimated number of concurrent users (based on request patterns)",
        )
        
        self.total_sessions = Counter(
            "lemonade_total_sessions",
            "Total number of sessions (estimated from unique request patterns)",
        )
        
        self.active_requests = Gauge(
            "lemonade_active_requests",
            "Estimated number of active requests",
        )
        
        # llama.cpp backend metrics (per model)
        self.llamacpp_prompt_tokens_total = Counter(
            "lemonade_llamacpp_prompt_tokens_total",
            "Total prompt tokens processed by llama.cpp backend",
            ["model_name", "backend_url"],
        )
        
        self.llamacpp_tokens_predicted_total = Counter(
            "lemonade_llamacpp_tokens_predicted_total",
            "Total generation tokens from llama.cpp backend",
            ["model_name", "backend_url"],
        )
        
        self.llamacpp_prompt_throughput = Gauge(
            "lemonade_llamacpp_prompt_tokens_per_second",
            "Prompt processing throughput from llama.cpp backend",
            ["model_name", "backend_url"],
        )
        
        self.llamacpp_predicted_throughput = Gauge(
            "lemonade_llamacpp_predicted_tokens_per_second",
            "Generation throughput from llama.cpp backend",
            ["model_name", "backend_url"],
        )
        
        self.llamacpp_requests_processing = Gauge(
            "lemonade_llamacpp_requests_processing",
            "Number of requests currently processing in llama.cpp backend",
            ["model_name", "backend_url"],
        )
        
        self.llamacpp_requests_deferred = Gauge(
            "lemonade_llamacpp_requests_deferred",
            "Number of requests deferred in llama.cpp backend",
            ["model_name", "backend_url"],
        )
        
        self.llamacpp_n_decode_total = Counter(
            "lemonade_llamacpp_n_decode_total",
            "Total llama_decode() calls in llama.cpp backend",
            ["model_name", "backend_url"],
        )
        
        self.llamacpp_tokens_predicted_seconds_total = Counter(
            "lemonade_llamacpp_tokens_predicted_seconds_total",
            "Total generation processing time in seconds from llama.cpp backend",
            ["model_name", "backend_url"],
        )
        
        # Track last values for counters to calculate increments
        self.llamacpp_last_values = {}  # {backend_url: {metric_name: value}}

    def fetch_stats(self) -> Optional[Dict]:
        """Fetch stats from Lemonade Server."""
        try:
            # Use curl via subprocess since Python HTTP libraries timeout
            # but curl works reliably
            result = subprocess.run(
                ["curl", "-s", "-m", "5", "--fail", self.stats_url],
                capture_output=True,
                text=True,
                timeout=10
            )
            if result.returncode == 0:
                return json.loads(result.stdout)
            return None
        except (subprocess.TimeoutExpired, json.JSONDecodeError, FileNotFoundError) as e:
            # Log errors
            print(f"Error fetching stats from {self.stats_url}: {e}", file=sys.stderr)
            return None
        except Exception as e:
            # Only log errors occasionally to avoid spam
            # The metrics will show server_up=0 when this fails
            return None

    def fetch_health(self) -> Optional[Dict]:
        """Fetch health status from Lemonade Server."""
        try:
            # Use curl via subprocess since Python HTTP libraries timeout
            # but curl works reliably
            result = subprocess.run(
                ["curl", "-s", "-m", "5", "--fail", self.health_url],
                capture_output=True,
                text=True,
                timeout=10
            )
            if result.returncode == 0:
                return json.loads(result.stdout)
            return None
        except (subprocess.TimeoutExpired, json.JSONDecodeError, FileNotFoundError) as e:
            # Log errors
            print(f"Error fetching health from {self.health_url}: {e}", file=sys.stderr)
            return None
        except Exception as e:
            # Only log errors occasionally to avoid spam
            return None

    def fetch_llamacpp_metrics(self, backend_url: str) -> Optional[str]:
        """Fetch Prometheus metrics from llama.cpp backend server."""
        try:
            # Convert backend_url from http://127.0.0.1:8003/v1 to http://127.0.0.1:8003/metrics
            metrics_url = backend_url.replace("/v1", "") + "/metrics"
            response = requests.get(metrics_url, timeout=5)
            response.raise_for_status()
            return response.text
        except Exception as e:
            # Backend might not have metrics enabled or might be down
            return None

    def parse_prometheus_metrics(self, metrics_text: str) -> Dict[str, float]:
        """Parse Prometheus format metrics text into a dictionary."""
        metrics = {}
        for line in metrics_text.split("\n"):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            # Parse format: metric_name value
            # or: metric_name{labels} value
            parts = line.split()
            if len(parts) >= 2:
                metric_name = parts[0].split("{")[0]  # Remove labels if present
                try:
                    value = float(parts[1])
                    metrics[metric_name] = value
                except ValueError:
                    continue
        return metrics

    def update_metrics(self):
        """Update Prometheus metrics from Lemonade Server."""
        current_time = time.time()
        
        # Skip if updated recently
        if current_time - self.last_update < self.update_interval:
            return
        
        stats = self.fetch_stats()
        health = self.fetch_health()
        
        with self.metrics_lock:
            # Server availability
            server_up = 1 if (stats is not None or health is not None) else 0
            self.server_up.set(server_up)
            
            if stats:
                # Performance metrics
                tps = stats.get("tokens_per_second")
                if tps is not None:
                    self.tokens_per_second.set(tps)
                
                ttft = stats.get("time_to_first_token")
                if ttft is not None:
                    self.time_to_first_token.set(ttft)
                
                # Token counts - Lemonade returns per-request values
                # Set as gauges for last request values
                input_tokens = stats.get("input_tokens")
                if input_tokens is not None:
                    self.input_tokens_last.set(input_tokens)
                    # Track cumulative for counter
                    if input_tokens > self.cumulative_input_tokens:
                        increment = input_tokens - self.cumulative_input_tokens
                        self.input_tokens_total.inc(increment)
                        self.cumulative_input_tokens = input_tokens
                
                output_tokens = stats.get("output_tokens")
                if output_tokens is not None:
                    self.output_tokens_last.set(output_tokens)
                    if output_tokens > self.cumulative_output_tokens:
                        increment = output_tokens - self.cumulative_output_tokens
                        self.output_tokens_total.inc(increment)
                        self.cumulative_output_tokens = output_tokens
                
                prompt_tokens = stats.get("prompt_tokens")
                # input_tokens already retrieved above
                if prompt_tokens is not None:
                    self.prompt_tokens_last.set(prompt_tokens)
                    if prompt_tokens > self.cumulative_prompt_tokens:
                        increment = prompt_tokens - self.cumulative_prompt_tokens
                        self.prompt_tokens_total.inc(increment)
                        self.cumulative_prompt_tokens = prompt_tokens
                    
                    # Calculate cache metrics
                    if input_tokens is not None and prompt_tokens > 0:
                        cached_tokens = max(0, prompt_tokens - input_tokens)
                        self.cached_tokens_last.set(cached_tokens)
                        # Cache hit rate: fraction of prompt that was cached
                        cache_hit_rate = cached_tokens / prompt_tokens
                        self.cache_hit_rate.set(cache_hit_rate)
                    elif prompt_tokens == 0:
                        # No prompt tokens, so no cache
                        self.cached_tokens_last.set(0)
                        self.cache_hit_rate.set(0)
                
                # Decode token times (histogram)
                # Note: Backend doesn't provide per-token decode times yet,
                # so we estimate from tokens_per_second
                # Create a hash of current stats to detect if this is a new request
                stats_hash = hash((
                    stats.get("input_tokens", 0),
                    stats.get("output_tokens", 0),
                    stats.get("tokens_per_second", 0),
                    tuple(stats.get("decode_token_times", []))
                ))
                
                # Only process decode times if this is a new request (different stats)
                if stats_hash != self.last_stats_hash:
                    self.last_stats_hash = stats_hash
                    self.total_requests += 1
                    
                    # Track this request for concurrent user estimation
                    # Estimate request duration: TTFT + (output_tokens / tokens_per_second)
                    output_tokens = stats.get("output_tokens", 0)
                    tps = stats.get("tokens_per_second")
                    ttft = stats.get("time_to_first_token", 0)
                    
                    if tps and tps > 0 and output_tokens > 0:
                        request_duration = ttft + (output_tokens / tps)
                    elif ttft > 0:
                        request_duration = ttft + 1.0  # Fallback estimate
                    else:
                        request_duration = 2.0  # Default estimate
                    
                    # Record request timestamp and duration
                    self.request_timestamps.append((current_time, request_duration))
                    self.last_request_time = current_time
                    
                    # Increment total sessions counter (each new request = new session)
                    self.total_sessions.inc()
                    
                    # Clean up old request timestamps (older than 60 seconds)
                    cutoff_time = current_time - 60
                    self.request_timestamps = [
                        (ts, dur) for ts, dur in self.request_timestamps
                        if ts + dur > cutoff_time
                    ]
                    
                    # Estimate concurrent users: requests that are still "active"
                    # A request is active if its start time + duration > current time
                    active_count = sum(
                        1 for ts, dur in self.request_timestamps
                        if ts + dur > current_time
                    )
                    self.active_requests.set(active_count)
                    
                    # Estimate concurrent users: similar to active requests but
                    # consider requests within a sliding window
                    # Use requests from last 30 seconds as proxy for concurrent users
                    window_start = current_time - 30
                    recent_requests = [
                        (ts, dur) for ts, dur in self.request_timestamps
                        if ts >= window_start
                    ]
                    # Estimate concurrent users as number of overlapping requests
                    concurrent_estimate = len(recent_requests)
                    self.concurrent_users.set(concurrent_estimate)
                    
                    decode_times = stats.get("decode_token_times", [])
                    
                    if decode_times:
                        # If backend provides actual decode times, use them
                        for dt in decode_times:
                            if dt > 0:
                                self.decode_token_time.observe(dt)
                    elif output_tokens > 0 and tps and tps > 0:
                        # Estimate per-token decode time from tokens_per_second
                        # Each token took approximately 1/tps seconds
                        estimated_decode_time = 1.0 / tps
                        # Record this estimated time for each output token
                        for _ in range(output_tokens):
                            self.decode_token_time.observe(estimated_decode_time)
            
            if health:
                # Health metrics
                all_models = health.get("all_models_loaded", [])
                self.models_loaded.set(len(all_models))
                
                # Clear previous model info labels
                # Note: prometheus_client doesn't have a direct way to clear labels,
                # so we'll set all known models to 1 and rely on Prometheus to handle
                # stale metrics
                if all_models:
                    for model in all_models:
                        model_name = model.get("model_name", "unknown")
                        model_type = model.get("type", "unknown")
                        device = model.get("device", "unknown")
                        self.model_info.labels(
                            model_name=model_name,
                            type=model_type,
                            device=device,
                        ).set(1)
                
                # Max models limits
                max_models = health.get("max_models", {})
                if max_models:
                    for model_type, limit in max_models.items():
                        self.max_models.labels(type=model_type).set(limit)
                
                # Fetch llama.cpp metrics for all llamacpp backends
                for model in all_models:
                    model_name = model.get("model_name", "unknown")
                    recipe = model.get("recipe", "")
                    backend_url = model.get("backend_url", "")
                    if recipe == "llamacpp" and backend_url:
                        self.update_llamacpp_metrics(model_name, backend_url)
            
            # Update timestamp
            self.last_update_time.set(current_time)
            self.last_update = current_time

    def update_llamacpp_metrics(self, model_name: str, backend_url: str):
        """Update llama.cpp metrics from backend server."""
        metrics_text = self.fetch_llamacpp_metrics(backend_url)
        if not metrics_text:
            return
        
        metrics = self.parse_prometheus_metrics(metrics_text)
        
        # Get or initialize last values for this backend
        if backend_url not in self.llamacpp_last_values:
            self.llamacpp_last_values[backend_url] = {}
        last_values = self.llamacpp_last_values[backend_url]
        
        # Update counters (calculate increments)
        prompt_tokens = metrics.get("llamacpp:prompt_tokens_total", 0)
        if prompt_tokens is not None:
            last_prompt = last_values.get("prompt_tokens_total", 0)
            if prompt_tokens > last_prompt:
                increment = prompt_tokens - last_prompt
                self.llamacpp_prompt_tokens_total.labels(
                    model_name=model_name, backend_url=backend_url
                ).inc(increment)
                last_values["prompt_tokens_total"] = prompt_tokens
        
        tokens_predicted = metrics.get("llamacpp:tokens_predicted_total", 0)
        if tokens_predicted is not None:
            last_predicted = last_values.get("tokens_predicted_total", 0)
            if tokens_predicted > last_predicted:
                increment = tokens_predicted - last_predicted
                self.llamacpp_tokens_predicted_total.labels(
                    model_name=model_name, backend_url=backend_url
                ).inc(increment)
                last_values["tokens_predicted_total"] = tokens_predicted
        
        n_decode = metrics.get("llamacpp:n_decode_total", 0)
        if n_decode is not None:
            last_decode = last_values.get("n_decode_total", 0)
            if n_decode > last_decode:
                increment = n_decode - last_decode
                self.llamacpp_n_decode_total.labels(
                    model_name=model_name, backend_url=backend_url
                ).inc(increment)
                last_values["n_decode_total"] = n_decode
        
        tokens_predicted_seconds = metrics.get("llamacpp:tokens_predicted_seconds_total", 0)
        if tokens_predicted_seconds is not None:
            last_seconds = last_values.get("tokens_predicted_seconds_total", 0)
            if tokens_predicted_seconds > last_seconds:
                increment = tokens_predicted_seconds - last_seconds
                self.llamacpp_tokens_predicted_seconds_total.labels(
                    model_name=model_name, backend_url=backend_url
                ).inc(increment)
                last_values["tokens_predicted_seconds_total"] = tokens_predicted_seconds
        
        # Update gauges (direct values)
        prompt_throughput = metrics.get("llamacpp:prompt_tokens_seconds")
        if prompt_throughput is not None:
            self.llamacpp_prompt_throughput.labels(
                model_name=model_name, backend_url=backend_url
            ).set(prompt_throughput)
        
        predicted_throughput = metrics.get("llamacpp:predicted_tokens_seconds")
        if predicted_throughput is not None:
            self.llamacpp_predicted_throughput.labels(
                model_name=model_name, backend_url=backend_url
            ).set(predicted_throughput)
        
        requests_processing = metrics.get("llamacpp:requests_processing")
        if requests_processing is not None:
            self.llamacpp_requests_processing.labels(
                model_name=model_name, backend_url=backend_url
            ).set(requests_processing)
        
        requests_deferred = metrics.get("llamacpp:requests_deferred")
        if requests_deferred is not None:
            self.llamacpp_requests_deferred.labels(
                model_name=model_name, backend_url=backend_url
            ).set(requests_deferred)


def update_metrics_loop(exporter: LemonadeExporter):
    """Background thread to update metrics periodically."""
    while True:
        try:
            exporter.update_metrics()
            time.sleep(exporter.update_interval)
        except Exception as e:
            print(f"Error updating metrics: {e}", file=sys.stderr)
            time.sleep(exporter.update_interval)


def main():
    """Main entry point."""
    import threading
    
    parser = argparse.ArgumentParser(
        description="Prometheus exporter for Lemonade Server metrics"
    )
    parser.add_argument(
        "--lemonade-url",
        default="http://localhost:8000",
        help="Lemonade Server URL (default: http://localhost:8000)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=9091,
        help="Port to listen on (default: 9091)",
    )
    parser.add_argument(
        "--host",
        default="0.0.0.0",
        help="Host to bind to (default: 0.0.0.0)",
    )

    args = parser.parse_args()

    exporter = LemonadeExporter(lemonade_url=args.lemonade_url)

    # Test connection (non-blocking, with timeout)
    print(f"Connecting to Lemonade Server at {args.lemonade_url}...")
    try:
        health = exporter.fetch_health()
        if health is None:
            print(
                f"Warning: Could not connect to Lemonade Server at {args.lemonade_url}",
                file=sys.stderr,
            )
            print("Exporter will start but metrics may be unavailable.", file=sys.stderr)
        else:
            print("✓ Connected to Lemonade Server")
    except Exception as e:
        print(
            f"Warning: Connection test failed: {e}",
            file=sys.stderr,
        )
        print("Exporter will start but metrics may be unavailable.", file=sys.stderr)

    # Start Prometheus HTTP server first (uses prometheus_client library)
    print(f"Lemonade Prometheus Exporter listening on {args.host}:{args.port}")
    print(f"Metrics available at: http://{args.host}:{args.port}/metrics")
    print("Press Ctrl+C to stop")

    try:
        start_http_server(args.port, addr=args.host, registry=REGISTRY)
        
        # Start background thread to update metrics (after server starts)
        update_thread = threading.Thread(
            target=update_metrics_loop,
            args=(exporter,),
            daemon=True,
        )
        update_thread.start()

        # Initial metrics update (non-blocking, will happen in background)
        # Don't wait for it - let the background thread handle it
        # Keep main thread alive
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nShutting down...")
        sys.exit(0)

if __name__ == "__main__":
    main()
