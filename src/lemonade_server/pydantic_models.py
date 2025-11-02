import os
import platform
from typing import Optional, Union, List

from pydantic import BaseModel

DEFAULT_PORT = int(os.getenv("LEMONADE_PORT", "8000"))
DEFAULT_HOST = os.getenv("LEMONADE_HOST", "localhost")
DEFAULT_LOG_LEVEL = os.getenv("LEMONADE_LOG_LEVEL", "info")


# Platform-aware default backend selection
def _get_default_llamacpp_backend():
    """
    Get the default llamacpp backend based on the current platform.
    """
    # Allow environment variable override
    env_backend = os.getenv("LEMONADE_LLAMACPP")
    if env_backend:
        return env_backend

    # Platform-specific defaults: use metal for Apple Silicon, vulkan for everything else
    if platform.system() == "Darwin" and platform.machine().lower() in [
        "arm64",
        "aarch64",
    ]:
        return "metal"
    return "vulkan"


DEFAULT_LLAMACPP_BACKEND = _get_default_llamacpp_backend()
DEFAULT_CTX_SIZE = int(os.getenv("LEMONADE_CTX_SIZE", "4096"))


class LoadConfig(BaseModel):
    """
    Configuration for loading a language model.

    Specifies the model checkpoint, generation parameters,
    and hardware/framework configuration (recipe) for model loading.
    """

    model_name: str
    checkpoint: Optional[str] = None
    recipe: Optional[str] = None
    # Indicates whether the model is a reasoning model, like DeepSeek
    reasoning: Optional[bool] = False
    # Indicates whether the model is a vision model with image processing capabilities
    vision: Optional[bool] = False
    # Indicates which Multimodal Projector (mmproj) file to use
    mmproj: Optional[str] = None


class CompletionRequest(BaseModel):
    """
    Request model for text completion API endpoint.

    Contains a prompt, a model identifier, and a streaming
    flag to control response delivery.
    """

    prompt: str
    model: str
    echo: bool = False
    stream: bool = False
    logprobs: int | None = False
    stop: list[str] | str | None = None
    temperature: float | None = None
    repeat_penalty: float | None = None
    top_k: int | None = None
    top_p: float | None = None
    max_tokens: int | None = None
    enable_thinking: bool | None = True


class ChatCompletionRequest(BaseModel):
    """
    Request model for chat completion API endpoint.

    Contains a list of chat messages, a model identifier,
    and a streaming flag to control response delivery.
    """

    messages: list[dict]
    model: str
    stream: bool = False
    logprobs: int | None = False
    stop: list[str] | str | None = None
    temperature: float | None = None
    repeat_penalty: float | None = None
    top_k: int | None = None
    top_p: float | None = None
    tools: list[dict] | None = None
    max_tokens: int | None = None
    max_completion_tokens: int | None = None
    response_format: dict | None = None
    enable_thinking: bool | None = True


class EmbeddingsRequest(BaseModel):
    """
    Request model for embeddings API endpoint.

    Generates embeddings for the provided input text or tokens.
    """

    input: Union[str, List]
    model: Optional[str] = None
    encoding_format: Optional[str] = "float"  # "float" or "base64"


class RerankingRequest(BaseModel):
    """
    Request model for reranking API endpoint.

    Reranks a list of documents based on their relevance to a query.
    """

    query: str
    documents: List[str]
    model: str


class ResponsesRequest(BaseModel):
    """
    Request model for responses API endpoint.
    """

    input: list[dict] | str
    model: str
    max_output_tokens: int | None = None
    temperature: float | None = None
    repeat_penalty: float | None = None
    top_k: int | None = None
    top_p: float | None = None
    stream: bool = False
    enable_thinking: bool | None = True


class PullConfig(LoadConfig):
    """
    Pull and load have the same fields.
    """


class DeleteConfig(BaseModel):
    """
    Configuration for deleting a supported LLM.
    """

    model_name: str


class LogLevelConfig(BaseModel):
    """
    Configuration for log-level setting.
    """

    level: str
