"""
Signal Executor - Parallel signal evaluation for semantic routing

Executes security signals (jailbreak, PII) in parallel, then routing
signals (keywords, complexity) to determine the best model for a request.
"""

import logging
import time
from concurrent.futures import ThreadPoolExecutor, as_completed, TimeoutError
from typing import Dict, Any, List, Optional, Callable
from dataclasses import dataclass, field

logger = logging.getLogger(__name__)


@dataclass
class SignalResult:
    """Result from evaluating a single signal"""

    name: str
    signal_type: str
    detected: bool = False
    score: float = 0.0
    action: Optional[str] = None  # "block", "redirect", None
    target_model: Optional[str] = None
    reason: str = ""
    latency_ms: float = 0.0
    error: Optional[str] = None
    metadata: Dict[str, Any] = field(default_factory=dict)


class SignalExecutor:
    """Executes signals with proper parallelization"""

    def __init__(self, max_workers: int = 4, timeout_ms: int = 500):
        """
        Initialize signal executor

        Args:
            max_workers: Max parallel threads for signal evaluation
            timeout_ms: Timeout per signal in milliseconds
        """
        self.max_workers = max_workers
        self.timeout_sec = timeout_ms / 1000.0

    def execute_signal(
        self,
        signal_name: str,
        signal_type: str,
        signal_fn: Callable[[], Dict[str, Any]],
        action: Optional[str] = None,
        target_model: Optional[str] = None,
    ) -> SignalResult:
        """
        Execute a single signal with timing

        Args:
            signal_name: Name of the signal
            signal_type: Type of signal (jailbreak, pii, keywords, complexity)
            signal_fn: Function that evaluates the signal (no args)
            action: Action if signal triggers (block, redirect)
            target_model: Target model if redirect action

        Returns:
            SignalResult with detection status and metadata
        """
        start = time.perf_counter()
        try:
            result = signal_fn()
            latency_ms = (time.perf_counter() - start) * 1000

            # Determine if signal triggered based on result
            detected = result.get("detected", False) or result.get("match", False)

            return SignalResult(
                name=signal_name,
                signal_type=signal_type,
                detected=detected,
                score=result.get("score", 0.0),
                action=action if detected else None,
                target_model=target_model if detected else None,
                reason=result.get("reason", ""),
                latency_ms=latency_ms,
                metadata=result,
            )
        except Exception as e:
            latency_ms = (time.perf_counter() - start) * 1000
            logger.error(f"Signal {signal_name} failed: {e}")
            return SignalResult(
                name=signal_name,
                signal_type=signal_type,
                error=str(e),
                latency_ms=latency_ms,
            )

    def execute_parallel(
        self, signals: List[Dict[str, Any]]
    ) -> Dict[str, SignalResult]:
        """
        Execute multiple signals in parallel

        Args:
            signals: List of signal configs, each with:
                - name: str
                - type: str
                - fn: Callable
                - action: Optional[str]
                - target_model: Optional[str]

        Returns:
            Dict mapping signal name to SignalResult
        """
        results = {}

        with ThreadPoolExecutor(max_workers=self.max_workers) as executor:
            future_to_signal = {
                executor.submit(
                    self.execute_signal,
                    sig["name"],
                    sig["type"],
                    sig["fn"],
                    sig.get("action"),
                    sig.get("target_model"),
                ): sig["name"]
                for sig in signals
            }

            for future in as_completed(future_to_signal, timeout=self.timeout_sec * 2):
                signal_name = future_to_signal[future]
                try:
                    result = future.result(timeout=self.timeout_sec)
                    results[signal_name] = result
                except TimeoutError:
                    logger.warning(f"Signal {signal_name} timed out")
                    results[signal_name] = SignalResult(
                        name=signal_name, signal_type="unknown", error="timeout"
                    )
                except Exception as e:
                    logger.error(f"Signal {signal_name} failed: {e}")
                    results[signal_name] = SignalResult(
                        name=signal_name, signal_type="unknown", error=str(e)
                    )

        return results

    def execute_security_then_routing(
        self,
        security_signals: List[Dict[str, Any]],
        routing_signals: List[Dict[str, Any]],
    ) -> Dict[str, Any]:
        """
        Phase 1: Run security signals (jailbreak, PII) in parallel
        Phase 2: If no block, run routing signals (keywords, complexity)

        Args:
            security_signals: Signals that can block requests
            routing_signals: Signals that determine model routing

        Returns:
            Final routing decision:
            {
                "action": "allow" | "block" | "redirect",
                "model": str | None,
                "reason": str,
                "signals": Dict[str, SignalResult],
                "latency_ms": float
            }
        """
        start = time.perf_counter()
        all_signals = {}

        # Phase 1: Security signals in parallel
        if security_signals:
            security_results = self.execute_parallel(security_signals)
            all_signals.update(security_results)

            # Check for any blocking signal
            for name, result in security_results.items():
                if result.detected and result.action == "block":
                    return {
                        "action": "block",
                        "model": None,
                        "reason": result.reason or f"Blocked by {name}",
                        "signals": {
                            k: self._result_to_dict(v) for k, v in all_signals.items()
                        },
                        "latency_ms": (time.perf_counter() - start) * 1000,
                        "blocked_by": name,
                    }

        # Phase 2: Routing signals (can run in parallel too)
        if routing_signals:
            routing_results = self.execute_parallel(routing_signals)
            all_signals.update(routing_results)

            # Find highest priority redirect
            # Priority is determined by order in routing_signals list
            for sig_config in routing_signals:
                sig_name = sig_config["name"]
                if sig_name in routing_results:
                    result = routing_results[sig_name]
                    if (
                        result.detected
                        and result.action == "redirect"
                        and result.target_model
                    ):
                        return {
                            "action": "redirect",
                            "model": result.target_model,
                            "reason": result.reason or f"Routed by {sig_name}",
                            "signals": {
                                k: self._result_to_dict(v)
                                for k, v in all_signals.items()
                            },
                            "latency_ms": (time.perf_counter() - start) * 1000,
                            "routed_by": sig_name,
                        }

        # No blocking or redirect - allow with no model override
        return {
            "action": "allow",
            "model": None,
            "reason": "No routing rules matched",
            "signals": {k: self._result_to_dict(v) for k, v in all_signals.items()},
            "latency_ms": (time.perf_counter() - start) * 1000,
        }

    def _result_to_dict(self, result: SignalResult) -> Dict[str, Any]:
        """Convert SignalResult to dict for JSON serialization"""
        return {
            "name": result.name,
            "type": result.signal_type,
            "detected": result.detected,
            "score": result.score,
            "action": result.action,
            "target_model": result.target_model,
            "reason": result.reason,
            "latency_ms": result.latency_ms,
            "error": result.error,
        }
