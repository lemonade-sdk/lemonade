"""Pydantic models for request/response handling."""

from typing import Optional, List, Dict, Any, Union
from pydantic import BaseModel


class ChatMessage(BaseModel):
    """A chat message."""

    role: str
    content: str


class ChatCompletionRequest(BaseModel):
    """Request for chat completion."""

    messages: List[ChatMessage]
    model: Optional[str] = "llama-model"
    temperature: Optional[float] = 0.7
    max_tokens: Optional[int] = 512
    stream: Optional[bool] = False
    stop: Optional[Union[str, List[str]]] = None


class CompletionRequest(BaseModel):
    """Request for text completion."""

    prompt: str
    model: Optional[str] = "llama-model"
    temperature: Optional[float] = 0.7
    max_tokens: Optional[int] = 512
    stream: Optional[bool] = False
    stop: Optional[Union[str, List[str]]] = None


class ChatCompletionResponse(BaseModel):
    """Response for chat completion."""

    id: str
    object: str = "chat.completion"
    created: int
    model: str
    choices: List[Dict[str, Any]]
    usage: Optional[Dict[str, int]] = None


class CompletionResponse(BaseModel):
    """Response for text completion."""

    id: str
    object: str = "text_completion"
    created: int
    model: str
    choices: List[Dict[str, Any]]
    usage: Optional[Dict[str, int]] = None


class HealthResponse(BaseModel):
    """Health check response."""

    status: str
    model_loaded: Optional[str] = None


class ModelInfo(BaseModel):
    """Model information."""

    id: str
    object: str = "model"
    created: int
    owned_by: str = "lemonade-lean"
