"""
Embedding-based routing strategies using MMBERT models
Uses sentence-transformers and MMBERT models from Hugging Face
"""

import os
import logging
from typing import List, Dict, Any, Optional
from pathlib import Path
import numpy as np

logger = logging.getLogger(__name__)

# Global model cache (loaded once, reused across requests)
_model_cache: Dict[str, Any] = {}


class EmbeddingRouter:
    """Router using sentence-transformers embeddings for similarity matching"""

    def __init__(self, model_name: str, cache_dir: Optional[str] = None):
        """
        Initialize embedding router

        Args:
            model_name: Model name from Hugging Face (e.g., "all-MiniLM-L6-v2")
            cache_dir: Directory to cache models (default: use Hugging Face cache)
        """
        self.model_name = model_name
        self.cache_dir = cache_dir
        self.model = None

    def load_model(self):
        """Load sentence-transformers model"""
        if self.model_name in _model_cache:
            self.model = _model_cache[self.model_name]
            logger.debug(f"Using cached model: {self.model_name}")
            return

        try:
            from sentence_transformers import SentenceTransformer
        except ImportError:
            raise ImportError(
                "sentence-transformers not installed. "
                "Install with: pip install sentence-transformers"
            )

        logger.info(f"Loading embedding model: {self.model_name}")
        if self.cache_dir:
            self.model = SentenceTransformer(
                self.model_name, cache_folder=self.cache_dir
            )
        else:
            self.model = SentenceTransformer(self.model_name)
        _model_cache[self.model_name] = self.model

    def route(
        self, messages: List[Dict[str, str]], examples: List[str], threshold: float
    ) -> Dict[str, Any]:
        """
        Route based on semantic similarity to examples

        Args:
            messages: Chat messages
            examples: Example prompts defining the target domain
            threshold: Minimum cosine similarity to match (0.0-1.0)

        Returns:
            Routing decision with match status and similarity score
        """
        if self.model is None:
            self.load_model()

        prompt = " ".join(
            [msg.get("content", "") for msg in messages if isinstance(msg, dict)]
        )

        if not prompt:
            return {"match": False, "score": 0.0, "top_match": None, "reason": ""}

        prompt_embedding = self.model.encode([prompt])
        example_embeddings = self.model.encode(examples)

        from sklearn.metrics.pairwise import cosine_similarity

        similarities = cosine_similarity(prompt_embedding, example_embeddings)[0]

        max_sim = float(np.max(similarities))
        best_idx = int(np.argmax(similarities))
        top_match = examples[best_idx]

        match = max_sim >= threshold

        return {
            "match": match,
            "score": max_sim,
            "top_match": top_match,
            "reason": (
                f"Semantic match: '{top_match[:50]}...' (similarity: {max_sim:.3f})"
                if match
                else ""
            ),
        }


class MMBERTClassifier:
    """Classifier using MMBERT models from llm-semantic-router/models"""

    def __init__(self, model_path: str, cache_dir: Optional[str] = None):
        """
        Initialize MMBERT classifier

        Args:
            model_path: Path to model directory or Hugging Face model name
            cache_dir: Directory to cache models (default: use Hugging Face cache)
        """
        self.model_path = model_path
        self.cache_dir = cache_dir
        self.model = None
        self.tokenizer = None

    def load_model(self):
        """Load MMBERT model from Hugging Face"""
        if self.model_path in _model_cache:
            self.model, self.tokenizer = _model_cache[self.model_path]
            logger.debug(f"Using cached MMBERT model: {self.model_path}")
            return

        try:
            from transformers import AutoTokenizer, AutoModelForSequenceClassification
            import torch
        except ImportError:
            raise ImportError(
                "transformers and torch not installed. "
                "Install with: pip install transformers torch"
            )

        logger.info(f"Loading MMBERT model: {self.model_path}")

        if os.path.exists(self.model_path):
            model_dir = self.model_path
        else:
            if "/" in self.model_path:
                model_dir = self.model_path
            else:
                model_dir = f"llm-semantic-router/{self.model_path}"

        # Load model and convert to float32 to avoid dtype compatibility issues
        if self.cache_dir:
            self.tokenizer = AutoTokenizer.from_pretrained(
                model_dir, cache_dir=self.cache_dir
            )
            self.model = AutoModelForSequenceClassification.from_pretrained(
                model_dir, cache_dir=self.cache_dir
            )
        else:
            self.tokenizer = AutoTokenizer.from_pretrained(model_dir)
            self.model = AutoModelForSequenceClassification.from_pretrained(model_dir)
        # Convert to float32 for compatibility
        self.model = self.model.float()
        self.model.eval()

        _model_cache[self.model_path] = (self.model, self.tokenizer)

    def classify(
        self, messages: List[Dict[str, str]], threshold: float
    ) -> Dict[str, Any]:
        """
        Classify text using MMBERT model

        Args:
            messages: Chat messages
            threshold: Classification threshold (0.0-1.0)

        Returns:
            Classification result with score and label
        """
        if self.model is None:
            self.load_model()

        try:
            import torch
        except ImportError:
            raise ImportError("torch not installed")

        prompt = " ".join(
            [msg.get("content", "") for msg in messages if isinstance(msg, dict)]
        )

        if not prompt:
            return {"detected": False, "score": 0.0, "label": None, "reason": ""}

        inputs = self.tokenizer(
            prompt, return_tensors="pt", truncation=True, max_length=512
        )

        with torch.no_grad():
            outputs = self.model(**inputs)
            logits = outputs.logits
            probs = torch.softmax(logits, dim=-1)

        score = float(probs.max())
        label_idx = int(probs.argmax())
        label = self._map_label(label_idx)
        detected = score >= threshold

        return {
            "detected": detected,
            "score": score,
            "label": label,
            "label_idx": label_idx,
            "reason": (
                f"{self._get_classifier_name()}: {label} (confidence: {score:.3f})"
                if detected
                else ""
            ),
        }

    def _get_classifier_name(self) -> str:
        """Get human-readable classifier name"""
        if "jailbreak" in self.model_path.lower():
            return "Jailbreak"
        elif "pii" in self.model_path.lower():
            return "PII"
        elif "intent" in self.model_path.lower() or "domain" in self.model_path.lower():
            return "Domain"
        elif "feedback" in self.model_path.lower():
            return "Feedback"
        else:
            return "Classifier"

    def _map_label(self, label_idx: int) -> str:
        """Map label index to human-readable name"""
        return f"label_{label_idx}"


def detect_jailbreak_mmbert(
    messages: List[Dict[str, str]],
    model_path: str = "mmbert32k-jailbreak-detector-merged",
    threshold: float = 0.7,
) -> Dict[str, Any]:
    """
    Detect jailbreak using MMBERT classifier

    Args:
        messages: Chat messages
        model_path: Path to MMBERT jailbreak detector model
        threshold: Detection threshold (default: 0.7)

    Returns:
        Detection result with score and label
    """
    classifier = MMBERTClassifier(model_path)
    result = classifier.classify(messages, threshold)
    # Jailbreak is detected when label_idx == 1 (label_0 = benign, label_1 = jailbreak)
    result["detected"] = (
        result.get("label_idx", 0) == 1 and result["score"] >= threshold
    )
    return result


def detect_pii_mmbert(
    messages: List[Dict[str, str]],
    model_path: str = "mmbert32k-pii-detector-merged",
    threshold: float = 0.9,
) -> Dict[str, Any]:
    """
    Detect PII using MMBERT classifier

    Args:
        messages: Chat messages
        model_path: Path to MMBERT PII detector model
        threshold: Detection threshold (default: 0.9)

    Returns:
        Detection result with PII types and confidence
    """
    classifier = MMBERTClassifier(model_path)
    result = classifier.classify(messages, threshold)
    # PII is detected when label_idx != 0 (label_0 = O/no PII)
    result["detected"] = (
        result.get("label_idx", 0) != 0 and result["score"] >= threshold
    )
    return result


def classify_domain_mmbert(
    messages: List[Dict[str, str]],
    model_path: str = "mmbert32k-intent-classifier-merged",
    threshold: float = 0.5,
) -> Dict[str, Any]:
    """
    Classify domain using MMBERT intent classifier

    Args:
        messages: Chat messages
        model_path: Path to MMBERT intent/domain classifier
        threshold: Classification threshold (default: 0.5)

    Returns:
        Classification result with domain and confidence
    """
    classifier = MMBERTClassifier(model_path)
    return classifier.classify(messages, threshold)


def score_complexity_mmbert(
    messages: List[Dict[str, str]],
    embedding_model: str = "llm-semantic-router/mmbert-embed-32k-2d-matryoshka",
    hard_examples: Optional[List[str]] = None,
    easy_examples: Optional[List[str]] = None,
    threshold: float = 0.28,
) -> Dict[str, Any]:
    """
    Score complexity using MMBERT embeddings and prototype matching

    Args:
        messages: Chat messages
        embedding_model: MMBERT embedding model name
        hard_examples: Example hard/complex prompts
        easy_examples: Example easy/simple prompts
        threshold: Medium complexity threshold (default: 0.28)

    Returns:
        Complexity score and classification (low/medium/high)
    """
    if hard_examples is None:
        hard_examples = [
            "analyze the root cause step by step and compare multiple mitigation strategies",
            "design a system architecture that satisfies multiple conflicting constraints",
            "optimize this algorithm for time complexity and memory usage",
            "derive and prove this formula mathematically with formal justification",
            "debug this complex issue by tracing through the execution path",
        ]

    if easy_examples is None:
        easy_examples = [
            "what is the definition of this term",
            "give a quick one-sentence answer",
            "answer yes or no to this simple question",
            "what does this word mean",
            "who is this person",
        ]

    router = EmbeddingRouter(embedding_model)
    router.load_model()

    prompt = " ".join(
        [msg.get("content", "") for msg in messages if isinstance(msg, dict)]
    )

    if not prompt:
        return {"score": 0.0, "complexity": "low", "reason": ""}

    prompt_embedding = router.model.encode([prompt])
    hard_embeddings = router.model.encode(hard_examples)
    easy_embeddings = router.model.encode(easy_examples)

    from sklearn.metrics.pairwise import cosine_similarity

    hard_sims = cosine_similarity(prompt_embedding, hard_embeddings)[0]
    easy_sims = cosine_similarity(prompt_embedding, easy_embeddings)[0]

    hard_score = float(np.max(hard_sims))
    easy_score = float(np.max(easy_sims))

    # Normalized complexity score
    complexity_score = (hard_score - easy_score + 1.0) / 2.0

    if complexity_score < threshold:
        complexity = "low"
    elif complexity_score < 0.60:
        complexity = "medium"
    else:
        complexity = "high"

    return {
        "score": complexity_score,
        "complexity": complexity,
        "hard_similarity": hard_score,
        "easy_similarity": easy_score,
        "reason": f"Complexity: {complexity} (score: {complexity_score:.3f})",
    }
