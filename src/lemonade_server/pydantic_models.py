from typing import Optional, Union, List, Any

from pydantic import BaseModel

# Set to a high number to allow for interesting experiences in real apps
# Tests should use the max_new_tokens argument to set a lower value
DEFAULT_MAX_NEW_TOKENS = 1500


class LoadConfig(BaseModel):
    """
    Configuration for loading a language model.

    Specifies the model checkpoint, generation parameters,
    and hardware/framework configuration (recipe) for model loading.
    """

    model_name: str
    checkpoint: Optional[str] = None
    recipe: Optional[str] = None
    # Indicates the maximum prompt length allowed for that specific
    # checkpoint + recipe combination
    max_prompt_length: Optional[int] = None
    # Indicates whether the model is a reasoning model, like DeepSeek
    reasoning: Optional[bool] = False
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
    max_tokens: int | None = None


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
    tools: list[dict] | None = None
    max_tokens: int | None = None
    max_completion_tokens: int | None = None
    response_format: dict | None = None


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
    stream: bool = False


class PullConfig(LoadConfig):
    """
    Pull and load have the same fields.
    """


class DeleteConfig(BaseModel):
    """
    Configuration for deleting a supported LLM.
    """

    model_name: str
