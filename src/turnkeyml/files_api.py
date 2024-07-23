import time
import os
import glob
from typing import List, Dict, Optional, Union
import git
import turnkeyml.common.printing as printing
import turnkeyml.common.exceptions as exceptions
from turnkeyml.sequence import Sequence
import turnkeyml.cli.spawn as spawn
import turnkeyml.common.filesystem as fs
import turnkeyml.common.labels as labels_library
from turnkeyml.state import State

# The licensing for tqdm is confusing. Pending a legal scan,
# the following code provides tqdm to users who have installed
# it already, while being transparent to users who do not
# have tqdm installed.
try:
    from tqdm import tqdm
except ImportError:

    def tqdm(iterable, **kwargs):  # pylint: disable=unused-argument
        return iterable


def unpack_txt_inputs(input_files: List[str]) -> List[str]:
    """
    Replace txt inputs with models listed inside those files
    Note: This implementation allows for nested .txt files
    """
    txt_files_expanded = sum(
        [glob.glob(f) for f in input_files if f.endswith(".txt")], []
    )
    processed_files = []
    for input_string in txt_files_expanded:
        if not os.path.exists(input_string):
            raise exceptions.ArgError(
                f"{input_string} does not exist. Please verify the file."
            )

        with open(input_string, "r", encoding="utf-8") as file:
            lines = [line.strip() for line in file if line.strip() != ""]
            processed_files.extend(unpack_txt_inputs(lines))

    return processed_files + [f for f in input_files if not f.endswith(".txt")]


def evaluate_files(
    input_files: List[str],
    sequence: Union[Dict, Sequence] = None,
    cache_dir: str = fs.DEFAULT_CACHE_DIR,
    lean_cache: bool = False,
    labels: List[str] = None,
    use_slurm: bool = False,
    process_isolation: bool = False,
    timeout: Optional[int] = None,
):
    """
    Iterate over a list of input files, evaluating each one with the provided sequence.

    Args:
        input_files: each file in this list will be passed into the first tool in
            the provided build sequence.
        sequence: the build tools and their arguments used to act on the inputs.
        cache_dir: Directory to use as the cache for this build. Output files
            from this build will be stored at cache_dir/build_name/
        lean_cache: delete build artifacts from the cache after the build has completed.
        lables: if provided, only input files that are marked with these labels will be
            passed into the sequence; the other input files will be skipped.
        use_slurm: evaluate each input file as its own slurm job (requires slurm to be)
            set up in advance on your system.
        process_isolation: evaluate each input file in a subprocess. If one subprocess
            fails, this function will move on to the next input file.
        timeout: in slurm or process isolation modes, the evaluation of each input file
            will be canceled if it exceeds this timeout value (in seconds).
    """

    # Replace .txt files with the models listed inside them
    input_files = unpack_txt_inputs(input_files)

    # Iterate through each string in the input_files list
    regular_files = []
    for input_string in input_files:
        if not any(char in input_string for char in "*?[]"):
            regular_files.append(input_string)

    # Create a list of files that don't exist on the filesystem
    # Skip the files with "::" as hashes will be decoded later
    non_existent_files = [
        file for file in regular_files if not os.path.exists(file) and "::" not in file
    ]
    if non_existent_files:
        raise exceptions.ArgError(
            f"{non_existent_files} do not exist, please verify if the file(s) exists."
        )

    # Make sure that `timeout` is only being used with `process_isolation` or `use_slurm`
    # And then set a default for timeout if the user didn't set a value
    if timeout is not None:
        if not use_slurm and not process_isolation:
            raise exceptions.ArgError(
                "The `timeout` argument is only allowed when slurm "
                "or process isolation mode is activated."
            )

        timeout_to_use = timeout
    else:
        timeout_to_use = spawn.DEFAULT_TIMEOUT_SECONDS

    # Convert regular expressions in input files argument
    # into full file paths (e.g., [*.py] -> [a.py, b.py] )
    input_files_expanded = fs.expand_inputs(input_files)

    # Make sure the cache directory exists
    fs.make_cache_dir(cache_dir)

    # Force the user to specify a legal cache dir in NFS if they are using slurm
    if cache_dir == fs.DEFAULT_CACHE_DIR and use_slurm:
        printing.log_warning(
            "Using the default cache directory when using Slurm will cause your cached "
            "files to only be available at the Slurm node. If this is not the behavior "
            "you desired, please se a --cache-dir that is accessible by both the slurm "
            "node and your local machine."
        )

    # Get list containing only file names
    clean_file_names = [
        fs.decode_input_arg(file_name)[0] for file_name in input_files_expanded
    ]

    # Validate that the files have supported file extensions
    # Note: We are not checking for .txt files here as those were previously handled
    for file_name in clean_file_names:
        if (
            not file_name.endswith(".py")
            and not file_name.endswith(".onnx")
            and not file_name.endswith("state.yaml")
        ):
            raise exceptions.ArgError(
                f"File extension must be .py, .onnx, or .txt (got {file_name})"
            )

    if use_slurm:
        jobs = spawn.slurm_jobs_in_queue()
        if len(jobs) > 0:
            printing.log_warning(f"There are already slurm jobs in your queue: {jobs}")
            printing.log_info(
                "Suggest quitting turnkey, running 'scancel -u $USER' and trying again."
            )

    use_progress_bar = len(input_files_expanded) > 1

    for file_path_encoded in tqdm(input_files_expanded, disable=not use_progress_bar):

        printing.log_info(f"Running turnkey on {file_path_encoded}")

        file_path_absolute, targets, encoded_input = fs.decode_input_arg(
            file_path_encoded
        )

        file_labels = fs.read_labels(file_path_absolute)

        build_name = fs.get_build_name(
            fs.clean_file_name(file_path_absolute),
            file_labels,
            targets[0] if len(targets) > 0 else None,
        )

        # Skip a file if the required_labels are not a subset of the script_labels.
        if labels:
            # Labels argument is not supported for ONNX files or cached builds
            if file_path_absolute.endswith(".onnx") or file_path_absolute.endswith(
                ".yaml"
            ):
                raise ValueError(
                    "The labels argument is not supported for .onnx files, got",
                    file_path_absolute,
                )
            required_labels = labels_library.to_dict(labels)
            if not labels_library.is_subset(required_labels, file_labels):
                continue

        if use_slurm or process_isolation:
            spawn.run_turnkey(
                build_name=build_name,
                sequence=sequence,
                file_name=encoded_input,
                use_slurm=use_slurm,
                process_isolation=process_isolation,
                timeout=timeout_to_use,
                lean_cache=lean_cache,
                cache_dir=cache_dir,
            )

        else:
            # Forward the selected input to the first tool in the sequence
            first_tool_args = next(iter(sequence.tools.values()))
            first_tool_args.append("--input")
            first_tool_args.append(file_path_encoded)

            # Collection of statistics that the sequence instance should save
            # to the stats file
            stats_to_save = {}

            # Save lables info
            if fs.Keys.AUTHOR in file_labels:
                stats_to_save[fs.Keys.AUTHOR] = file_labels[fs.Keys.AUTHOR][0]
            if fs.Keys.TASK in file_labels:
                stats_to_save[fs.Keys.TASK] = file_labels[fs.Keys.TASK][0]

            # Save all of the lables in one place
            stats_to_save[fs.Keys.LABELS] = file_labels

            # If the input script is a built-in TurnkeyML model, make a note of
            # which one
            if os.path.abspath(fs.MODELS_DIR) in os.path.abspath(file_path_absolute):
                try:
                    # If this turnkey installation is in a git repo, use the
                    # specific git hash
                    git_repo = git.Repo(search_parent_directories=True)
                    git_hash = git_repo.head.object.hexsha
                except git.exc.InvalidGitRepositoryError:
                    # If we aren't in a git repo (e.g., PyPI package), point the user back to main
                    git_hash = "main"

                relative_path = file_path_absolute.replace(
                    fs.MODELS_DIR,
                    f"https://github.com/onnx/turnkeyml/tree/{git_hash}/models",
                ).replace("\\", "/")
                stats_to_save[fs.Keys.MODEL_SCRIPT] = relative_path

            state = State(
                cache_dir=cache_dir,
                build_name=build_name,
                sequence_info=sequence.info,
            )
            sequence.launch(
                state,
                lean_cache=lean_cache,
                stats_to_save=stats_to_save,
            )

    # Wait until all the Slurm jobs are done
    if use_slurm:
        while len(spawn.slurm_jobs_in_queue()) != 0:
            print(
                f"Waiting: {len(spawn.slurm_jobs_in_queue())} "
                f"jobs left in queue: {spawn.slurm_jobs_in_queue()}"
            )
            time.sleep(5)
