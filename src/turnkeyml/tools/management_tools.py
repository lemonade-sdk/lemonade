import argparse
import abc
from typing import List
import turnkeyml.common.filesystem as fs
import turnkeyml.common.exceptions as exp
import turnkeyml.common.printing as printing
from turnkeyml.tools.tool import ToolParser
from turnkeyml.version import __version__ as turnkey_version
from turnkeyml.common.system_info import get_system_info_dict
from turnkeyml.common.build import output_dir


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
            prog=f"turnkey {cls.unique_name}",
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
    Simply prints the version number of the turnkeyml installation.
    """

    unique_name = "version"

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Print the turnkeyml version number",
            add_help=add_help,
        )

        return parser

    def run(self, _):
        print(turnkey_version)


class Cache(ManagementTool):
    # pylint: disable=pointless-statement,f-string-without-interpolation
    f"""
    A set of functions for managing the turnkey build cache. The default
    cache location is {fs.DEFAULT_CACHE_DIR}, and can also be selected with
    the global --cache-dir option or the TURNKEY_CACHE_DIR environment variable.

    Users must set either "--all" or "--build-names" to let the tool
    know what builds to operate on.

    Users must also set one of the available actions (e.g., list, stats, etc.).

    That action will be applied to all selected builds.
    """

    unique_name = "cache"

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        # NOTE: `--cache-dir` is set as a global input to the turnkey CLI and
        # passed directly to the `run()` method

        parser = __class__.helpful_parser(
            short_description="Manage the turnkey build cache " f"",
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
                    "Try running `turnkey cache list` to see the builds in your build cache."
                )

        print()


class ModelsLocation(ManagementTool):
    """
    Prints the location of the turnkeyml built in models corpora.

    This is especially useful for when turnkey was installed from PyPI
    with `pip install turnkeyml`. Example usage in this context:
        models=$(turnkey models-location --quiet)
        turnkey -i $models/selftest/linear.py discover export-pytorch
    """

    unique_name = "models-location"

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Print the location of the built-in turnkeyml models",
            add_help=add_help,
        )

        parser.add_argument(
            "-q",
            "--quiet",
            action="store_true",
            help="Print only the file path, with no other text",
        )

        return parser

    def run(self, _, quiet: bool = False):
        if quiet:
            print(fs.MODELS_DIR)
        else:
            printing.log_info(f"The models directory is: {fs.MODELS_DIR}")


class SystemInfo(ManagementTool):
    """
    Prints system information for the turnkeyml installation.
    """

    unique_name = "system-info"

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Print system information",
            add_help=add_help,
        )

        return parser

    @staticmethod
    def pretty_print(my_dict: dict, level=0):
        for k, v in my_dict.items():
            if isinstance(v, dict):
                print("    " * level + f"{k}:")
                SystemInfo.pretty_print(v, level + 1)
            elif isinstance(v, list):
                print("    " * level + f"{k}:")
                for item in v:
                    print("    " * (level + 1) + f"{item}")
            else:
                print("    " * level + f"{k}: {v}")

    def run(self, _):
        system_info_dict = get_system_info_dict()
        self.pretty_print(system_info_dict)
