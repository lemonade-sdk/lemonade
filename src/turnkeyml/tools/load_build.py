import pathlib
import copy
import argparse
from typing import Union, Dict
from turnkeyml.tools import FirstTool
import turnkeyml.common.exceptions as exp
import turnkeyml.common.build as build
import turnkeyml.common.filesystem as fs
import turnkeyml.common.status as status
from turnkeyml.state import State, load_state
import turnkeyml.common.printing as printing
from turnkeyml.version import __version__ as turnkey_version

skip_policy_default = "attempted"


def _decode_version_number(version: str) -> Dict[str, int]:
    numbers = [int(x) for x in version.split(".")]
    return {"major": numbers[0], "minor": numbers[1], "patch": numbers[0]}


class LoadBuild(FirstTool):
    """
    Tool that loads a build from a previous usage of TurnkeyML and passes
    its saved State on to the next tool in the sequence.

    Works best with build State that is complete on disk.

    For example:
    - State that references an ONNX file is a good target, because the ONNX file can
        be loaded from disk.
    - State that references a PyTorch model in memory is a poor target, because
        that PyTorch model will not be available when the State file is loaded
        from disk.

    Expected inputs:
    - Input file is a *_state.yaml file in a turnkey cache build directory

    Outputs:
     - State has the contents of the state.yaml file of the target build.
    """

    unique_name = "load-build"

    def __init__(self):
        super().__init__(monitor_message="Loading cached build")

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Load build state from the cache",
            add_help=add_help,
        )

        parser.add_argument(
            "--skip-policy",
            choices=[skip_policy_default, "failed", "successful", "none"],
            help="Sets the policy for skipping evaluation attempts "
            f"(defaults to {skip_policy_default})."
            "`attempted` means to skip any previously-attempted evaluation, "
            "whether it succeeded or failed."
            "`failed` skips evaluations that have already failed once."
            "`successful` skips evaluations that have already succeeded."
            "`none` will attempt all evaluations, regardless of whether "
            "they were previously attempted.",
            required=False,
            default=skip_policy_default,
        )

        return parser

    def run(self, state: State, input: str = "", skip_policy=skip_policy_default):

        # Extract the cache directory, build directory, and build name from the input
        source_build_dir = pathlib.Path(input).parent
        source_build_dir_name = source_build_dir.name
        source_cache_dir = source_build_dir.parent.parent

        # Make sure that the target yaml file is actually the state of a turnkey build
        if not fs.is_build_dir(source_cache_dir, source_build_dir_name):
            raise exp.CacheError(
                f"No build found at path: {input}. "
                "Try running `turnkey cache --list --all` to see the builds in your build cache."
            )

        # Record the new sequence's information so that we can append it to the
        # loaded build's sequence information later
        new_sequence_info = state.sequence_info

        # Load the cached build
        printing.log_info(f"Attempting to load: {input}")
        state = load_state(state_path=input)

        # Point the state.cache_dir and state.results at the current location of the state.yaml file
        # This is important because state.yaml may have moved, and by default all paths in
        # state.yaml will be the original location, not any new location
        state.cache_dir = str(source_cache_dir)
        state.results = fs.rebase_cache_dir(
            input_path=state.results,
            build_name=source_build_dir_name,
            new_cache_dir=source_cache_dir,
        )

        # Record the sequence used for the loaded build so that we examine it later
        # However, API users wont have populated this, so skip it if sequence_info
        # isn't populated
        if state.sequence_info is not None:
            prior_selected_sequence = list(state.sequence_info.keys())
        else:
            prior_selected_sequence = None

        # Raise an exception if there is a version mismatch between the installed
        # version of turnkey and the version of turnkey used to create the loaded
        # build
        current_version_decoded = _decode_version_number(turnkey_version)
        state_version_decoded = _decode_version_number(state.turnkey_version)
        out_of_date: Union[str, bool] = False
        if current_version_decoded["major"] > state_version_decoded["major"]:
            out_of_date = "major"
        elif current_version_decoded["minor"] > state_version_decoded["minor"]:
            out_of_date = "minor"

        if out_of_date:
            raise exp.SkipBuild(
                f"Your build {state.build_name} was previously built against "
                f"turnkey version {state.turnkey_version}, "
                f"however you are now using turnkey version {turnkey_version}. "
                "The previous build is "
                f"incompatible with this version of turnkey, as indicated by the {out_of_date} "
                "version number changing. See **docs/versioning.md** for details."
            )

        # Append the sequence of this build to the sequence of the loaded build.
        # so that the stats file reflects the complete set of Tools that have been
        # attempted on this build
        if prior_selected_sequence is not None:
            stats = fs.Stats(state.cache_dir, state.build_name)
            combined_selected_sequence = copy.deepcopy(prior_selected_sequence)
            for new_tool, new_tool_args in new_sequence_info.items():
                combined_selected_sequence.append(new_tool)
                state.sequence_info[new_tool] = new_tool_args
            stats.save_stat(
                fs.Keys.SELECTED_SEQUENCE_OF_TOOLS, combined_selected_sequence
            )

        # Apply the skip policy by raising a SkipBuild exception
        # if the pre-existing build status doesn't meet certain criteria
        if (
            prior_selected_sequence is None
            or self.__class__.unique_name not in prior_selected_sequence
        ):
            if state.build_status != build.FunctionStatus.SUCCESSFUL:
                if skip_policy == "attempted" or skip_policy == "failed":
                    raise exp.SkipBuild(
                        f"Skipping {state.build_name} because it has a "
                        f"status of {state.build_status} and the skip policy "
                        f"is set to {skip_policy}."
                    )
                else:
                    # Issue a warning to users if they loaded an unsuccessful build
                    # This is a warning, instead of an exception, to allow for the case
                    # where a Tool is being re-attempted under different conditions (e.g.,
                    # re-attempting a benchmark after a system restart).
                    if state.build_status != build.FunctionStatus.SUCCESSFUL:
                        print(f"Warning: loaded build status is {state.build_status}")
        else:
            if skip_policy == "attempted":
                raise exp.SkipBuild(
                    f"Skipping {state.build_name} because it was previously attempted "
                    f"and the skip policy is set to {skip_policy}"
                )
            elif (
                skip_policy == "successful"
                and state.build_status == build.FunctionStatus.SUCCESSFUL
            ):
                raise exp.SkipBuild(
                    f"Skipping {state.build_name} because it was previously successfully "
                    f"attempted and the skip policy is set to {skip_policy}"
                )
            elif (
                skip_policy == "failed"
                and state.build_status != build.FunctionStatus.SUCCESSFUL
            ):
                raise exp.SkipBuild(
                    f"Skipping {state.build_name} because it was previously "
                    f"unsuccessfully attempted and the skip policy is set to {skip_policy}"
                )
            elif skip_policy == "none":
                # Skip policy of "none" means we should never skip over a build
                pass
            else:
                # The skip condition is not met, so we will continue
                pass

        # Mark the build status as incomplete now that we have re-opened it
        state.build_status = build.FunctionStatus.INCOMPLETE

        # Create a UniqueInvocationInfo and ModelInfo so that we can display status
        # at the end of the sequence
        status.add_to_state(state=state, name=input, model=input)

        return state
