import os
import sys
from typing import Dict, Optional, Any
import yaml
import turnkeyml.common.build as build
import turnkeyml.common.filesystem as fs
from turnkeyml.version import __version__ as turnkey_version
from turnkeyml.memory_tracker import MemoryTracker


def _is_nice_to_write(value):
    """
    Checks whether a value is nice to write to YAML.
    Returns True if the value is a string, int, float, bool, list, dict, or tuple.
    Returns False otherwise.
    """
    if isinstance(value, (str, int, float, bool)):
        return True
    elif isinstance(value, list) or isinstance(value, tuple):
        # Check if all elements in the list are nice to write
        return all(_is_nice_to_write(item) for item in value)
    elif isinstance(value, dict):
        # Check if all values in the dictionary are nice to write
        return all(_is_nice_to_write(item) for item in value.values())
    return False


def _sanitize_for_yaml(input_dict: Dict) -> Dict:
    """
    Creates a new dictionary containing only nice-to-write values
    from the original dictionary.
    """
    result = {}
    for key, value in input_dict.items():
        if _is_nice_to_write(value):
            result[key] = value
    return result


class State:
    """
    The State class is meant to carry build state, starting with the user's
    initial arguments, through each build Tool in the Sequence, and finally
    to the disk, where it is used to assess cache hits.

    State is initialized with the key members that are shared by every build,
    and reasonable default values are assigned as appropriate.

    Tool developers can also add any members they wish. To get or set an
    attribute, reference it as an attribute:
        1. get: `my_variable = state.attribute_name`
        2. set: `state.attribute_name = my_variable`

    Build State can be saved and loaded from disk in the form of a state.yaml file
    via State.save() and load_state(), respectively. Note that while State can
    contain members of any type, only YAML-safe members (str, int, bool, float,
    list, dict, tuple) will be saved and loaded.
    """

    def __init__(
        self,
        cache_dir: str,
        build_name: Optional[str] = None,
        sequence_info: Dict[str, Dict] = None,
        **kwargs,
    ):

        # The default model name is the name of the python file that calls build_model()
        if build_name is None:
            build_name = os.path.basename(sys.argv[0])

        # Support "~" in the cache_dir argument
        parsed_cache_dir = os.path.expanduser(cache_dir)

        # Save settings as State members
        self.cache_dir = parsed_cache_dir
        self.build_name = build_name
        self.sequence_info = sequence_info
        self.turnkey_version = turnkey_version
        self.build_status = build.FunctionStatus.NOT_STARTED
        self.downcast_applied = False
        self.uid = build.unique_id()
        self.results = None
        self.memory_tracker = MemoryTracker()

        # Store any additional kwargs as members
        for key, value in kwargs.items():
            self.__dict__[key] = value

    def __setattr__(self, name: str, value: Any) -> None:
        """
        Tool developers can add a new member to State by simply
        assigning it as an attribute, i.e., `state.new_member = value`.
        """
        return super().__setattr__(name, value)

    def save_stat(self, key: str, value):
        """
        Save statistics to an yaml file in the build directory
        """

        stats = fs.Stats(self.cache_dir, self.build_name)
        stats.save_stat(key, value)

    def save_sub_stat(self, parent_key: str, key: str, value):
        """
        Save statistics to an yaml file in the build directory
        """

        stats = fs.Stats(self.cache_dir, self.build_name)
        stats.save_sub_stat(parent_key, key, value)

    def save(self):
        """
        Save all YAML-friendly members to disk as a state.yaml file.

        Note that `model` and `inputs` will typically not be saved since
        they are typically in non-YAML-friendly types such as `torch.nn.Module`
        and `torch.tensor`.
        """

        state_to_save = _sanitize_for_yaml(vars(self))

        # Create a build directory in the cache
        fs.make_build_dir(self.cache_dir, self.build_name)

        with open(
            build.state_file(self.cache_dir, self.build_name),
            "w",
            encoding="utf8",
        ) as outfile:
            yaml.dump(state_to_save, outfile)


def load_state(
    cache_dir=None,
    build_name=None,
    state_path=None,
) -> State:
    """
    Read a state.yaml file corresponding to a specific build in a specific
    cache, and use its contents to initialize a State instance.
    """

    if state_path is not None:
        file_path = state_path
    elif build_name is not None and cache_dir is not None:
        file_path = build.state_file(cache_dir, build_name)
    else:
        raise ValueError(
            "This function requires either build_name and cache_dir to be set, "
            "or state_path to be set, not both or neither"
        )

    state_dict = build.load_yaml(file_path)

    return State(**state_dict)
