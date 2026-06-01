"""
Simple Semantic Router for Lemonade

Main routing interface that takes a prompt and config, evaluates signals,
and returns the appropriate model to route to.
"""

import logging
from typing import Dict, Any, List, Optional
import yaml

from .signal_executor import SignalExecutor
from .strategies.bm25 import route_by_bm25
from .strategies.embedding import (
    detect_jailbreak_mmbert,
    detect_pii_mmbert,
    score_complexity_mmbert,
)

logger = logging.getLogger(__name__)

# Default complexity examples (used if not specified in config)
DEFAULT_HARD_EXAMPLES = [
    "analyze the root cause step by step and compare multiple mitigation strategies",
    "derive and prove this formula or theorem mathematically with formal justification",
    "explain the tradeoffs between different approaches in depth and recommend the best one",
    "implement a complete solution handling all edge cases and failure modes",
    "design a system architecture that satisfies multiple conflicting constraints",
    "debug this complex issue by tracing through the execution path and identifying the fault",
    "optimize this algorithm or system for time complexity and memory usage",
    "synthesize findings from multiple sources and draw a well-reasoned conclusion",
]

DEFAULT_EASY_EXAMPLES = [
    "what is the definition of this term",
    "give a quick one-sentence answer",
    "briefly summarize this in a few words",
    "what does this word or acronym mean",
    "answer yes or no to this simple question",
    "translate this short phrase into another language",
    "convert this value from one unit to another",
    "what is the capital city of this country",
]


def route(prompt: str, config: Dict[str, Any]) -> str:
    """
    Main routing function - simple interface

    Args:
        prompt: User's message text
        config: Parsed YAML config dict

    Returns:
        model_name: The model to route to (or None to use default)
    """
    result = route_with_metadata(prompt, config)
    return result.get("model") or config.get("default_model")


def route_with_metadata(prompt: str, config: Dict[str, Any]) -> Dict[str, Any]:
    """
    Extended routing with full metadata

    Args:
        prompt: User's message text
        config: Parsed YAML config dict

    Returns:
        {
            "action": "allow|block|redirect",
            "model": "model_name" or None,
            "reason": "human readable explanation",
            "signals": {...},
            "latency_ms": float
        }
    """
    if not config.get("enabled", True):
        return {
            "action": "allow",
            "model": None,
            "reason": "Routing disabled",
            "signals": {},
            "latency_ms": 0,
        }

    # Convert prompt to messages format expected by strategies
    messages = [{"role": "user", "content": prompt}]

    # Build signal functions from config
    signals_config = config.get("signals", {})
    models_config = config.get("models", [])

    # Create model lookup for resolving model names to IDs
    model_lookup = {m["name"]: m["id"] for m in models_config}

    # Build security signals
    security_signals = []
    routing_signals = []

    # Jailbreak signal
    jailbreak_cfg = signals_config.get("jailbreak", {})
    if jailbreak_cfg.get("enabled", True):
        threshold = jailbreak_cfg.get("threshold", 0.7)
        security_signals.append(
            {
                "name": "jailbreak",
                "type": "jailbreak",
                "fn": lambda t=threshold: detect_jailbreak_mmbert(
                    messages, threshold=t
                ),
                "action": "block",
            }
        )

    # PII signal
    pii_cfg = signals_config.get("pii", {})
    if pii_cfg.get("enabled", True):
        threshold = pii_cfg.get("threshold", 0.9)
        security_signals.append(
            {
                "name": "pii",
                "type": "pii",
                "fn": lambda t=threshold: detect_pii_mmbert(messages, threshold=t),
                "action": "block",
            }
        )

    # Keyword signals
    keywords_cfg = signals_config.get("keywords", {})
    for keyword_name, keyword_config in keywords_cfg.items():
        corpus = keyword_config.get("corpus", [])
        threshold = keyword_config.get("threshold", 0.25)
        target_name = keyword_config.get("target")
        target_model = model_lookup.get(target_name, target_name)

        if corpus and target_model:
            routing_signals.append(
                {
                    "name": f"keywords.{keyword_name}",
                    "type": "keywords",
                    "fn": lambda c=corpus, t=threshold: route_by_bm25(messages, c, t),
                    "action": "redirect",
                    "target_model": target_model,
                }
            )

    # Complexity signal
    complexity_cfg = signals_config.get("complexity", {})
    if complexity_cfg.get("enabled", True):
        hard_examples = complexity_cfg.get("hard_examples", DEFAULT_HARD_EXAMPLES)
        easy_examples = complexity_cfg.get("easy_examples", DEFAULT_EASY_EXAMPLES)

        # Get model mapping for complexity levels
        low_target = complexity_cfg.get("low")
        medium_target = complexity_cfg.get("medium")
        high_target = complexity_cfg.get("high")

        low_model = model_lookup.get(low_target, low_target)
        medium_model = model_lookup.get(medium_target, medium_target)
        high_model = model_lookup.get(high_target, high_target)

        def complexity_router():
            result = score_complexity_mmbert(
                messages, hard_examples=hard_examples, easy_examples=easy_examples
            )
            complexity = result.get("complexity", "low")

            # Determine target based on complexity level
            if complexity == "high" and high_model:
                result["target_model"] = high_model
                result["match"] = True
            elif complexity == "medium" and medium_model:
                result["target_model"] = medium_model
                result["match"] = True
            elif complexity == "low" and low_model:
                result["target_model"] = low_model
                result["match"] = True
            else:
                result["match"] = False

            return result

        # Only add if we have at least one target model
        if high_model or medium_model:
            routing_signals.append(
                {
                    "name": "complexity",
                    "type": "complexity",
                    "fn": complexity_router,
                    "action": "redirect",
                    "target_model": high_model
                    or medium_model,  # Will be overwritten by fn
                }
            )

    # Execute signals (default 5s timeout to handle first-time model loading)
    executor = SignalExecutor(
        max_workers=4,
        timeout_ms=config.get("settings", {}).get("signal_timeout_ms", 5000),
    )

    result = executor.execute_security_then_routing(security_signals, routing_signals)

    # If routing signals returned a target from complexity, use it
    if result["action"] == "redirect":
        # Check if complexity signal set a specific target
        complexity_signal = result.get("signals", {}).get("complexity", {})
        if complexity_signal and "metadata" in complexity_signal:
            specific_target = complexity_signal.get("metadata", {}).get("target_model")
            if specific_target:
                result["model"] = specific_target

    return result


def validate_config(config: Dict[str, Any]) -> tuple:
    """
    Validate routing config

    Args:
        config: Config dict to validate

    Returns:
        (is_valid: bool, errors: List[str])
    """
    errors = []

    # Check required fields
    if "version" not in config:
        errors.append("Missing 'version' field")

    # Validate models
    models = config.get("models", [])
    if not models:
        errors.append("No models defined")
    else:
        for i, model in enumerate(models):
            if "name" not in model:
                errors.append(f"Model {i}: missing 'name'")
            if "id" not in model:
                errors.append(f"Model {i}: missing 'id'")

    # Validate signals
    signals = config.get("signals", {})

    # Validate keyword signals
    keywords = signals.get("keywords", {})
    for name, kw_config in keywords.items():
        if not kw_config.get("corpus"):
            errors.append(f"Keyword signal '{name}': empty corpus")
        if not kw_config.get("target"):
            errors.append(f"Keyword signal '{name}': no target model")

    # Validate complexity signal
    complexity = signals.get("complexity", {})
    if complexity.get("enabled", True):
        if not any(
            [complexity.get("low"), complexity.get("medium"), complexity.get("high")]
        ):
            errors.append("Complexity signal enabled but no target models defined")

    return (len(errors) == 0, errors)


def load_config(config_path: str) -> Dict[str, Any]:
    """
    Load config from YAML file

    Args:
        config_path: Path to YAML config file

    Returns:
        Parsed config dict
    """
    with open(config_path, "r") as f:
        return yaml.safe_load(f)


def get_default_config() -> Dict[str, Any]:
    """
    Get a minimal default config for testing

    Returns:
        Default config dict
    """
    return {
        "version": "1.0",
        "enabled": True,
        "default_model": None,
        "models": [],
        "signals": {
            "jailbreak": {
                "enabled": True,
                "threshold": 0.7,
            },
            "pii": {
                "enabled": True,
                "threshold": 0.9,
            },
            "keywords": {},
            "complexity": {
                "enabled": False,
            },
        },
        "settings": {
            "signal_timeout_ms": 500,
            "fail_open": True,
        },
    }


def get_sample_config() -> str:
    """
    Get a sample YAML config for users to start with

    Returns:
        Sample config as YAML string
    """
    return """# Lemonade Semantic Router Config
version: "1.0"
enabled: true
default_model: "Qwen3.5-9B-NoThinking"

models:
  - name: "local-small"
    id: "Qwen3.5-9B-NoThinking"
    type: local
  - name: "cloud-kimi"
    id: "fireworks.kimi-k2p6"
    type: cloud

signals:
  jailbreak:
    enabled: true
    threshold: 0.7

  pii:
    enabled: true
    threshold: 0.9

  keywords:
    complex_task:
      corpus:
        - "system design"
        - "software architecture"
        - "implement"
        - "refactor"
        - "algorithm"
        - "machine learning"
      threshold: 0.25
      target: "cloud-kimi"
    simple_query:
      corpus:
        - "what is"
        - "define"
        - "list"
        - "how do i"
      threshold: 0.15
      target: "local-small"

  complexity:
    enabled: true
    low: "local-small"
    medium: "local-small"
    high: "cloud-kimi"

settings:
  signal_timeout_ms: 500
  fail_open: true
"""
