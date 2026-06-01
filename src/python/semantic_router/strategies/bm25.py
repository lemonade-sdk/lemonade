"""
BM25-based routing strategy
Routes requests based on keyword relevance scoring
"""

from typing import List, Dict, Any
import numpy as np


def route_by_bm25(
    messages: List[Dict[str, str]], corpus: List[str], threshold: float
) -> Dict[str, Any]:
    """
    Route based on BM25 keyword matching

    Args:
        messages: List of chat messages with "role" and "content" keys
        corpus: List of keywords/phrases defining the target domain
        threshold: Minimum BM25 score to trigger routing (0.0-1.0)

    Returns:
        Dictionary with routing result:
        {
            "match": bool,
            "score": float,
            "top_match": str | None,
            "reason": str
        }
    """
    prompt = " ".join(
        [msg.get("content", "") for msg in messages if isinstance(msg, dict)]
    )
    query_tokens = prompt.lower().split()

    if not query_tokens:
        return {"match": False, "score": 0.0, "top_match": None, "reason": ""}

    corpus_tokens = [doc.lower().split() for doc in corpus]
    scores = _compute_bm25_scores(query_tokens, corpus_tokens)

    max_score = max(scores) if scores else 0.0
    best_idx = int(np.argmax(scores)) if scores else 0
    top_match = corpus[best_idx] if scores else None

    match = bool(max_score >= threshold)

    return {
        "match": match,
        "score": float(max_score),
        "top_match": top_match,
        "reason": (
            f"BM25 match: '{top_match}' (score: {max_score:.3f})" if match else ""
        ),
    }


def _compute_bm25_scores(
    query_tokens: List[str],
    corpus_tokens: List[List[str]],
    k1: float = 1.5,
    b: float = 0.75,
) -> List[float]:
    """
    Compute BM25 scores for query against corpus

    Args:
        query_tokens: Tokenized query
        corpus_tokens: List of tokenized corpus documents
        k1: BM25 k1 parameter (term frequency saturation)
        b: BM25 b parameter (length normalization)

    Returns:
        List of BM25 scores for each corpus document
    """
    if not corpus_tokens:
        return []

    N = len(corpus_tokens)
    avgdl = sum(len(doc) for doc in corpus_tokens) / N

    # Compute IDF for each query term
    idf = {}
    for term in query_tokens:
        df = sum(1 for doc in corpus_tokens if term in doc)
        idf[term] = np.log((N - df + 0.5) / (df + 0.5) + 1.0) if df > 0 else 0.0

    # Compute BM25 score for each document
    scores = []
    for doc in corpus_tokens:
        score = 0.0
        doc_len = len(doc)

        term_freqs = {}
        for term in doc:
            term_freqs[term] = term_freqs.get(term, 0) + 1

        for term in query_tokens:
            if term in term_freqs:
                tf = term_freqs[term]
                numerator = idf.get(term, 0.0) * tf * (k1 + 1)
                denominator = tf + k1 * (1 - b + b * doc_len / avgdl)
                score += numerator / denominator

        scores.append(score)

    # Normalize scores to 0-1 range
    if max(scores) > 0:
        scores = [s / max(scores) for s in scores]

    return scores
