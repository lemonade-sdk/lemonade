import argparse
import abc
import json
from typing import List
import lemonade.common.filesystem as fs
import lemonade.common.exceptions as exp
import lemonade.common.printing as printing
from lemonade.tools.tool import ToolParser
from lemonade.version import __version__ as lemonade_version
from lemonade.common.system_info import (
    get_system_info_dict,
    get_device_info_dict,
    get_system_info,
)
from lemonade.common.build import output_dir
import lemonade.cache as lemonade_cache


class ManagementTool(abc.ABC):
    """
    Intended for management functions, such as managing the cache
    or printing the version number.
    """

    unique_name: str

    @classmethod
    def helpful_parser(cls, short_description: str, **kwargs):
        epilog = (
            f"`{cls.unique_name}` is a Management Tool. It is intended to be invoked by itself "
            "(i.e., not as part of a sequence), to accomplish a utility function. "
        )

        return ToolParser(
            prog=f"lemonade {cls.unique_name}",
            short_description=short_description,
            description=cls.__doc__,
            epilog=epilog,
            **kwargs,
        )

    @staticmethod
    @abc.abstractmethod
    def parser() -> argparse.ArgumentParser:
        """
        Static method that returns an ArgumentParser that defines the command
        line interface for this Tool.
        """

    # pylint: disable=unused-argument
    def parse(self, args, known_only=True) -> argparse.Namespace:
        """
        Run the parser and return a Namespace of keyword arguments that the user
        passed to the Tool via the command line.

        Tools should extend this function only if they require specific parsing
        logic.

        Args:
            args: command line arguments passed from the CLI.
            known_only: this argument allows the CLI framework to
                incrementally parse complex commands.
        """

        if known_only:
            parsed_args = self.__class__.parser().parse_args(args)
        else:
            parsed_args, _ = self.__class__.parser().parse_known_args(args)

        return parsed_args

    @abc.abstractmethod
    def run(self, cache_dir: str):
        """
        Execute the functionality of the Tool.
        """

    def parse_and_run(self, cache_dir: str, args, known_only=True):
        """
        Helper function to parse CLI arguments into the args expected
        by run(), and then forward them into the run() method.
        """

        parsed_args = self.parse(args, known_only)
        self.run(cache_dir, **parsed_args.__dict__)


class Version(ManagementTool):
    """
    Simply prints the version number of the lemonade installation.
    """

    unique_name = "version"

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Print the lemonade version number",
            add_help=add_help,
        )

        return parser

    def run(self, _):
        print(lemonade_version)


class Cache(ManagementTool):
    # pylint: disable=pointless-statement,f-string-without-interpolation
    f"""
    A set of functions for managing the lemonade build cache. The default
    cache location is {lemonade_cache.DEFAULT_CACHE_DIR}, and can also be
    selected with
    the global --cache-dir option or the LEMONADE_CACHE_DIR environment variable.

    Users must set either "--all" or "--build-names" to let the tool
    know what builds to operate on.

    Users must also set one of the available actions (e.g., list, stats, etc.).

    That action will be applied to all selected builds.
    """

    unique_name = "cache"

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        # NOTE: `--cache-dir` is set as a global input to the lemonade CLI and
        # passed directly to the `run()` method

        parser = __class__.helpful_parser(
            short_description="Manage the build cache " f"",
            add_help=add_help,
        )

        build_selection_group = parser.add_mutually_exclusive_group(required=True)

        build_selection_group.add_argument(
            "-b",
            "--build-names",
            nargs="+",
            help="Name of the specific build(s) to be operated upon, within the cache directory",
        )

        build_selection_group.add_argument(
            "-a",
            "--all",
            dest="all_builds",
            help="Operate on all the builds in the cache",
            action="store_true",
        )

        action_group = parser.add_mutually_exclusive_group(required=True)

        action_group.add_argument(
            "-l",
            "--list",
            dest="list_builds",
            action="store_true",
            help="List all of the builds in the cache",
        )

        action_group.add_argument(
            "-s",
            "--stats",
            action="store_true",
            help="Print the collected stats for the selected build(s)",
        )

        action_group.add_argument(
            "--delete",
            action="store_true",
            help="Permanently delete the selected build(s)",
        )

        action_group.add_argument(
            "--clean",
            action="store_true",
            help="Remove the build artifacts from the selected build(s)",
        )

        return parser

    def run(
        self,
        cache_dir: str,
        all_builds: bool = False,
        build_names: List[str] = None,
        list_builds: bool = False,
        stats: bool = False,
        delete: bool = False,
        clean: bool = False,
    ):
        fs.check_cache_dir(cache_dir)

        if all_builds and build_names:
            raise ValueError(
                "all_builds and build_names are mutually exclusive, "
                "but both are used in this call."
            )
        elif all_builds:
            builds = fs.get_available_builds(cache_dir)
        elif build_names:
            builds = build_names
        else:
            raise ValueError(
                "Either all_builds or build_names must be set, "
                "but this call sets neither."
            )

        # Print a nice heading
        printing.log_info(f"Operating on cache directory {cache_dir}")

        if not builds:
            printing.log_warning("No builds found.")

        for build in builds:
            build_path = output_dir(cache_dir, build_name=build)
            if fs.is_build_dir(cache_dir, build):
                # Run actions on the build
                # These actions are intended to be mutually exclusive, so we
                # use an if-elif block in order from least to most destructive
                if list_builds:
                    print(build)
                elif stats:
                    fs.print_yaml_file(fs.Stats(cache_dir, build).file, "stats")
                elif clean:
                    fs.clean_output_dir(cache_dir, build)
                    printing.log_info(f"Removed the build artifacts from: {build}")

                elif delete:
                    fs.rmdir(build_path)
                    printing.log_info(f"Deleted build: {build}")
            else:
                raise exp.CacheError(
                    f"No build found with name: {build}. "
                    "Try running `lemonade cache --list` to see the builds in your build cache."
                )

        print()


class SystemInfo(ManagementTool):
    """
    Prints system information for the lemonade installation.
    """

    unique_name = "system-info"

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Print system and device information",
            add_help=add_help,
        )

        parser.add_argument(
            "--format", choices=["table", "json"], default="table", help="Output format"
        )

        parser.add_argument(
            "--verbose",
            action="store_true",
            help="Show detailed system information",
        )

        return parser

    @staticmethod
    def pretty_print(my_dict: dict, level=0):
        for k, v in my_dict.items():
            if k == "available" and v is True:
                continue

            if isinstance(v, dict):
                # Special handling for device availability
                if v.get("available") is False:
                    error_msg = v.get("error", "Not available")
                    print("    " * level + f"{k}: {error_msg}")
                else:
                    print("    " * level + f"{k}:")
                    SystemInfo.pretty_print(v, level + 1)
            elif isinstance(v, list):
                print("    " * level + f"{k}:")
                for item in v:
                    if isinstance(item, dict):
                        SystemInfo.pretty_print(item, level + 1)
                        print()
                    else:
                        print("    " * (level + 1) + f"{item}")
            else:
                print("    " * level + f"{k}: {v}")

    def run(self, _, format="table", verbose=False):
        # Get basic system info
        system_info_dict = get_system_info_dict()

        # Always include devices
        system_info_dict["Devices"] = get_device_info_dict()

        # Filter out verbose-only information if not in verbose mode
        if not verbose:
            essential_keys = ["OS Version", "Processor", "Physical Memory", "Devices"]
            system_info_dict = {
                k: v for k, v in system_info_dict.items() if k in essential_keys
            }
        else:
            # In verbose mode, add Python packages at the end
            system_info = get_system_info()
            system_info_dict["Python Packages"] = system_info.get_python_packages()

        if format == "json":
            print(json.dumps(system_info_dict, indent=2))
        else:
            self.pretty_print(system_info_dict)


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
