import abc


class ModelAdapter(abc.ABC):
    """
    Base class for adapting an LLM to work with lemonade's standardized tools
    """

    def __init__(self):
        """
        Self-benchmarking ModelAdapters can store their results in the
        tokens_per_second and time_to_first_token members.
        """
        self.tokens_per_second = None
        self.time_to_first_token = None
        self.type = "generic"

    @abc.abstractmethod
    def generate(self, input_ids, max_new_tokens=512):
        """
        Generate is the primary method required by lemonade's accuracy tools

        We try to keep the signature here minimal to allow for maximum compatibility
        with recipe components, which themselves may not support a lot of arguments.
        """


class TokenizerAdapter(abc.ABC):
    """
    Base class for adapting an LLM's tokenizer to work with lemonade's standard tools
    """

    @abc.abstractmethod
    def __call__(self, prompt: str):
        """
        Args:
            prompt: text that should be encoded and passed to the LLM as input_ids

        Returns: input_ids
        """

    @abc.abstractmethod
    def decode(self, response) -> str:
        """
        Args:
            response: tokens from the LLM that should be decoded into text

        Returns: text response of the LLM
        """


class PassthroughTokenizerResult:
    """
    Data structure for holding a tokenizer result where the input_ids
    are packaged in a non-standard way, but we still want to adhere to
    standard interfaces (e.g., result.input_ids).

    For example: CLI-based tools that have their own internal tokenizer that
    isn't exposed to the user. In this case we can pass the prompt through as
    a string.
    """

    def __init__(self, prompt):
        self.input_ids = prompt


class PassthroughTokenizer(TokenizerAdapter):
    """
    Tokenizer adapter that forwards the prompt to input_ids as text,
    and then forwards a text LLM response through decode() as text.

    Useful for CLI-based tools that have their own internal tokenizer that
    isn't exposed to the user.
    """

    # pylint: disable=unused-argument
    def __call__(self, prompt: str, **kwargs):
        return PassthroughTokenizerResult(prompt)

    # pylint: disable=unused-argument
    def decode(self, response: str, **kwargs):
        return response
