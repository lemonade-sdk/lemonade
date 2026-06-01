"""
Lemonade Semantic Router

Intelligent prompt-based routing between local and cloud models.
Evaluates signals (jailbreak, PII, keywords, complexity) in parallel
and routes to the most appropriate model based on config.
"""

from .simple_router import route, route_with_metadata, validate_config
from .signal_executor import SignalExecutor

__all__ = ["route", "route_with_metadata", "validate_config", "SignalExecutor"]
__version__ = "0.1.0"
