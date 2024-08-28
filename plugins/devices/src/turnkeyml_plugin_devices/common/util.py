import json
import os
import subprocess
import pkgutil
import platform
import shutil
from typing import Tuple
import posixpath
import git

import turnkeyml.common.exceptions as exp
from turnkeyml.common.printing import log_info

import turnkeyml_plugin_devices.common.run.plugin_helpers as plugin_helpers

PLUGIN_DIR = os.path.dirname(pkgutil.get_loader("turnkeyml_plugin_devices").path)


def version_path(plugin_name: str):
    return os.path.join(PLUGIN_DIR, plugin_name, "version.json")


def get_versions(plugin_name: str):
    """Get correct version number for runtime and dependencies"""

    path = version_path(plugin_name)

    with open(path, "r", encoding="utf-8") as file:
        return json.load(file)


def get_version_key(plugin_name: str, version_key: str) -> str:
    with open(version_path(plugin_name), "r", encoding="utf-8") as file:
        versions = json.load(file)

    return versions[version_key]


def get_runtime_version(plugin_name: str) -> str:
    return get_version_key(plugin_name, "runtime_version")


def get_deps_dir(plugin_name: str):
    return os.path.join(PLUGIN_DIR, plugin_name, "deps")


def conda_and_env_path(conda_env_name: str) -> Tuple[str, str]:
    conda_path = os.getenv("CONDA_EXE")
    if conda_path is None:
        raise EnvironmentError(
            "CONDA_EXE environment variable not set."
            "Make sure Conda is properly installed."
        )

    # Normalize the path for Windows
    if platform.system() == "Windows":
        conda_path = os.path.normpath(conda_path)

    env_path = os.path.join(
        os.path.dirname(os.path.dirname(conda_path)), "envs", conda_env_name
    )

    return conda_path, env_path


def get_env_version_path(env_path) -> str:
    return os.path.join(env_path, "version.json")


def env_up_to_date(plugin_name: str, env_path: str) -> bool:

    # Check if the local plugin version matches the conda environment version and part
    env_version_path = get_env_version_path(env_path)
    local_version = get_runtime_version(plugin_name)
    if os.path.exists(env_version_path):
        with open(env_version_path, "r", encoding="utf-8") as file:
            env_versions = json.load(file)
        if local_version == env_versions["runtime_version"]:
            return True

    return False


def write_env_version(plugin_name: str, env_path: str):
    # Attach a version to the conda env created
    shutil.copy(version_path(plugin_name), get_env_version_path(env_path))


def get_directory_size(directory):
    total_size = 0
    for dirpath, _, filenames in os.walk(directory):
        for filename in filenames:
            filepath = os.path.join(dirpath, filename)
            total_size += os.path.getsize(filepath)
    return total_size


def lfs_pull(plugin_name: str):
    dependencies_dir = get_deps_dir(plugin_name)

    # For developers in editable mode, fetch the deps folder from git lfs
    # NOTE: for non-editable-mode (ie, `pip install` with the `-e`):
    #   `git lfs pull -I PLUGIN_PATH` needs to be manually called prior to `pip install`
    deps_size_bytes = get_directory_size(dependencies_dir)

    # LFS files have not been pulled if the deps folder is under 100KB
    # because the folder will only have LFS pointers in it, which are about 100B each
    plugin_path = posixpath.join(
        "plugins",
        "devices",
        "src",
        "turnkeyml_plugin_devices",
        plugin_name,
    )
    lfs_command = f'git lfs pull -I {plugin_path} -X " "'

    deps_folder_too_small_size_bytes = 100000
    # Only attempt this in a git repo; if this code is running somewhere else
    # like site-packages then this will be skipped
    with open(os.devnull, "w", encoding="utf-8") as d:
        is_git_repo = not bool(
            subprocess.call(
                "git rev-parse",
                shell=True,
                stdout=d,
                stderr=d,
                cwd=os.path.abspath(os.path.dirname(__file__)),
            )
        )

    if is_git_repo and deps_size_bytes < deps_folder_too_small_size_bytes:
        # Always run the LFS command from the git repo root
        git_repo = git.Repo(__file__, search_parent_directories=True)
        lfs_cwd = git_repo.git.rev_parse("--show-toplevel")

        print("Running:", lfs_command)
        print("With cwd:", lfs_cwd)
        subprocess.run(
            lfs_command,
            shell=True,
            # check=False because the next code block will raise a more helpful
            # exception if this subprocess doesn't work out
            check=False,
            cwd=lfs_cwd,
        )

    # If the deps size didn't change after the pull, that means the pull
    # silently failed. Raise a helpful exception.
    deps_size_bytes_post_pull = get_directory_size(dependencies_dir)
    if deps_size_bytes_post_pull < deps_folder_too_small_size_bytes:
        raise exp.EnvError(
            "The plugin dependencies have not been pulled from LFS "
            "If you are building from source you can try running this command: "
            f"`{lfs_command}`"
        )


def create_fresh_conda_env(conda_env_name: str, python_version: str, requirements=None):
    conda_path, env_path = conda_and_env_path(conda_env_name)

    # Create new environment from scratch
    log_info("Updating environment...")
    if os.path.exists(env_path):
        plugin_helpers.run_subprocess(
            [
                conda_path,
                "remove",
                "--name",
                conda_env_name,
                "--all",
                "-y",
            ]
        )

    plugin_helpers.run_subprocess(
        [
            conda_path,
            "create",
            "--name",
            conda_env_name,
            f"python={python_version}",
            "-y",
        ]
    )

    # Install requirements in the created environment
    if requirements is not None:
        for req in requirements:
            plugin_helpers.run_subprocess(
                [conda_path, "run", "--name", conda_env_name, "pip", "install", req]
            )
