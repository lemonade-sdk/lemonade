"""
Signal evaluation strategies for semantic routing.

Each strategy evaluates a specific signal type:
- bm25: Keyword matching using BM25 scoring
- embedding: MMBERT-based detection (jailbreak, PII, complexity)
"""

from .bm25 import route_by_bm25
from .embedding import (
    detect_jailbreak_mmbert,
    detect_pii_mmbert,
    score_complexity_mmbert,
    EmbeddingRouter,
)

__all__ = [
    "route_by_bm25",
    "detect_jailbreak_mmbert",
    "detect_pii_mmbert",
    "score_complexity_mmbert",
    "EmbeddingRouter",
]
