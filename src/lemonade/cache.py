import os
from datetime import datetime, timezone

# Allow an environment variable to override the default
# location for the build cache
if os.environ.get("LEMONADE_CACHE_DIR"):
    DEFAULT_CACHE_DIR = os.path.expanduser(os.environ.get("LEMONADE_CACHE_DIR"))
else:
    DEFAULT_CACHE_DIR = os.path.join(os.path.expanduser("~"), ".cache", "lemonade")


def checkpoint_to_model_name(checkpoint_name: str) -> str:
    """
    Get the model's name by stripping the author's name from the checkpoint name
    """

    return checkpoint_name.split("/")[1]


def get_timestamp() -> str:
    """
    Get a timestamp string in the format:
        <year>y_<month>m_<day>d_<hour>h_<minute>m_<second>s
    """
    # Get the current time in GMT
    current_time = datetime.now(timezone.utc)

    # Format the timestamp string
    timestamp = current_time.strftime("%Yy_%mm_%dd_%Hh_%Mm_%Ss")
    return timestamp


def build_name(input_name):
    """
    Name the lemonade build by concatenating these two factors:
        1. Sanitize the input name (typically a model checkpoint name) by
            replacing any `/` characters with `_`.
        2. Timestamp to ensure that builds in the same cache will not
            collide in the same build directory.
    """

    # Sanitize the input name
    input_name_sanitized = input_name.replace("/", "_")

    # Get the formatted timestamp string
    timestamp = get_timestamp()

    return f"{input_name_sanitized}_{timestamp}"


class Keys:
    MODEL = "model"
    PER_ITERATION_LATENCY = "per_iteration_latency"
    MEAN_LATENCY = "mean_latency"
    STD_DEV_LATENCY = "std_dev_latency"
    TOKEN_GENERATION_TOKENS_PER_SECOND = "token_generation_tokens_per_second"
    STD_DEV_TOKENS_PER_SECOND = "std_dev_tokens_per_second"
    SECONDS_TO_FIRST_TOKEN = "seconds_to_first_token"
    PREFILL_TOKENS_PER_SECOND = "prefill_tokens_per_second"
    STD_DEV_SECONDS_TO_FIRST_TOKEN = "std_dev_seconds_to_first_token"
    CHECKPOINT = "checkpoint"
    DTYPE = "dtype"
    PROMPT = "prompt"
    PROMPT_TOKENS = "prompt_tokens"
    RESPONSE = "response"
    RESPONSE_TOKENS = "response_tokens"
    RESPONSE_LENGTHS_HISTOGRAM = "response_lengths_histogram"
    CACHE_DIR = "cache_dir"
    DEVICE = "device"
    OGA_MODELS_SUBFOLDER = "oga_models_subfolder"
    MEMORY_USAGE_PLOT = "memory_usage_plot"
    MAX_MEMORY_USED_GB = "max_memory_used_GB"
    MAX_MEMORY_USED_GBYTE = "max_memory_used_gbyte"
