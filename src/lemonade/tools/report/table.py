from abc import ABC, abstractmethod
from datetime import datetime, timezone
import re
from typing import Tuple, Dict, List
import textwrap
from tabulate import tabulate
import turnkeyml.common.build as build
import turnkeyml.common.filesystem as fs
import turnkeyml.tools.report as tkml_report
from lemonade.cache import Keys
from lemonade.tools.huggingface_bench import HuggingfaceBench
from lemonade.tools.llamacpp_bench import LlamaCppBench
from lemonade.tools.mmlu import AccuracyMMLU
from lemonade.tools.ort_genai.oga_bench import OgaBench

# List of python packages for which to log the version
PYTHON_PACKAGES = ["onnxruntime", "transformers", "turnkeyml", "voe"]

# Key value in local build data dict
SW_VERSIONS = "sw_versions"

# Map datatype aliases to common names
dtype_aliases = {
    "fp32": "float32",
    "fp16": "float16",
}


################################################################################
# HELPER FUNCTIONS
################################################################################


def _to_list(x) -> List:
    """Puts item in list if it is not already a list"""
    if isinstance(x, list):
        return x
    return [x]


def _wrap(text: str, width: int) -> str:
    """Wraps text cleanly to specified width"""
    return "\n".join(textwrap.wrap(text, width=width))


def _merge_join(str1, str2) -> str:
    """Joins a pair of strings with \n as long as both are non-empty, else skips the \n"""
    return str1 + ("\n" if str1 and str2 else "") + str2


################################################################################
# CLASSES THAT DESCRIBE TEXT TABLE COLUMNS
################################################################################


# Table entry types
class TableColumn(ABC):

    default_wrap = 80

    @abstractmethod
    def get_str(self, build_stats: dict, lean=False) -> str:
        """Method used to return the string that goes in the table column for this build"""


class SimpleStat(TableColumn):
    """These are for statistics already declared by the tool or basic build stats"""

    def __init__(
        self,
        column_header,
        stat,
        format_str,
        align="center",
        omit_if_lean=False,
        wrap=None,
    ):
        self.column_header = column_header
        self.stat = stat
        self.format_str = format_str
        self.align = align
        self.omit_if_lean = omit_if_lean
        self.wrap = wrap or self.default_wrap

    def get_str(self, build_stats, lean=False):
        if lean and self.omit_if_lean:
            return None
        data = build_stats.get(self.stat, None)
        if data is None:
            return ""
        cell_str = "\n".join(
            [_wrap(f"{x:{self.format_str}}", self.wrap) for x in _to_list(data)]
        )
        return cell_str


class TimestampStat(SimpleStat):
    """These are for timestamp statistics already declared by the tool or basic build stats"""

    def get_str(self, build_stats, lean=False):
        if lean and self.omit_if_lean:
            return None
        data = build_stats.get(self.stat, None)
        if data is None:
            return "-"
        cell_str = data.strftime(self.format_str)
        return cell_str


class MultiStat(TableColumn):
    """
    These are for string-values statistics already declared by the tool or basic build stats.
    One or more stats will be put in the same cell.
    """

    def __init__(
        self,
        column_header,
        stats,
        format_str,
        align="center",
        omit_if_lean=False,
        wrap=None,
    ):
        self.column_header = column_header
        self.stats = _to_list(stats)
        self.format_str = format_str
        self.align = align
        self.omit_if_lean = omit_if_lean
        self.wrap = wrap or self.default_wrap

    def get_str(self, build_stats, lean=False):
        if lean and self.omit_if_lean:
            return None
        cell_str_list = []
        for stat in self.stats:
            if stat is None:
                cell_str_list.append("")  # will be a blank line
            else:
                data = build_stats.get(stat, None)
                if data is None:
                    cell_str_list.append("-")  # missing value
                else:
                    cell_str_list.append(_wrap(f"{data:{self.format_str}}", self.wrap))
        cell_str = "\n".join(cell_str_list)
        return cell_str


class StatWithSD(TableColumn):
    """These are for statistics already declared by the tool that have an
    accompanying standard deviation statistic"""

    def __init__(
        self,
        column_header,
        stat,
        sd_stat,
        format_str,
        align="center",
        omit_if_lean=False,
    ):
        self.column_header = column_header
        self.stat = stat
        self.sd_stat = sd_stat
        self.format_str = format_str
        self.align = align
        self.omit_if_lean = omit_if_lean

    def get_str(self, build_stats, lean=False):
        if lean and self.omit_if_lean:
            return None
        if not self.stat in build_stats:
            return ""
        data = build_stats[self.stat]
        sd_data = build_stats.get(self.sd_stat, None)
        if sd_data is None:
            data = _to_list(data)
            sd_data = [None] * len(data)
        cell_str = "\n".join(
            [
                (
                    f"{x:{self.format_str}} +/- {sd_x:{self.format_str}}"
                    if not sd_x is None
                    else f"{x:{self.format_str}}"
                )
                for x, sd_x in zip(_to_list(data), _to_list(sd_data))
            ]
        )
        return cell_str


class AdditionalStat(TableColumn):
    """These are for statistics not declared by the tool.  A regular expression is defined
    and all statistics matching the regular expression will be put in the cell"""

    def __init__(
        self,
        column_header,
        regexp,
        lean_regexp,
        format_str,
        align="center",
        omit_if_lean=False,
        wrap=None,
    ):
        self.column_header = column_header
        self.regexp = regexp
        self.lean_regexp = lean_regexp or regexp
        self.format_str = format_str
        self.align = align
        self.omit_if_lean = omit_if_lean
        self.wrap = wrap or self.default_wrap

    def get_str(self, build_stats, lean=False):
        if lean and self.omit_if_lean:
            return None
        # Find stats in build_stats that match the regexp for this column
        regexp = self.lean_regexp if lean else self.regexp
        stats = []
        for stat in build_stats.keys():
            if re.match(regexp, stat):
                stats.append(stat)
        # Construct the cell entry
        cell_entry = []
        for stat in stats:
            if stat.endswith("_units"):
                continue
            units = build_stats.get(stat + "_units", None)
            value = f"{build_stats[stat]:{self.format_str}}" + (
                " " + units if not units is None else ""
            )
            cell_entry += [_wrap(stat, self.wrap), value]
        return "\n".join(cell_entry)


################################################################################
# ABSTRACT BASE CLASS FOR DEFINING A TABLE
################################################################################


class Table(ABC):

    table_descriptor = {}

    def __init__(self):
        self.all_builds = []
        self.all_stats = []
        self.lean = False
        self.tools = None
        self.merge_test_fn = lambda b1, b2: False

    def find_builds(self, cache_dirs: List[str], model: str = None):
        """
        Finds all the folder names of all the builds in the given list of cache directories.
        Each (cache_dir, build_folder) tuple is appended to the all_builds list attribute.
        If a model string is given, then builds will only be saved if the build folder name
        contains the model string (case-insensitive).
        """
        self.all_builds = []
        for cache_dir in cache_dirs:
            # Get all of the directories within the build cache
            builds = fs.get_available_builds(cache_dir)

            # Filter out any that don't have the right model in the name
            if model is not None:
                builds = [
                    (cache_dir, build_name)
                    for build_name in builds
                    if model.lower() in build_name.lower()
                ]
            else:
                builds = [(cache_dir, build_name) for build_name in builds]

            self.all_builds += builds

    def load_stats(self):
        """
        Loads the build stats dict from each build into the all_stats list attribute,
        one dict per build.
        """
        self.all_stats = []

        # Add results from all user-provided cache folders
        for cache_dir, build_name in self.all_builds:
            model_stats = fs.Stats(cache_dir, build_name).stats

            if self.include_stats(model_stats):
                self.post_process_stats(model_stats)

    def include_stats(self, _model_stats: Dict) -> bool:
        """
        Returns True if stats from this model build should be part of the table
        """
        return True

    def post_process_stats(self, model_stats: Dict) -> Dict:
        """
        Create a dict of stats from these model stats and append to the all_stats list attribute.
        Make any necessary modifications along the way
        """
        evaluation_stats = {}
        for key, value in model_stats.items():
            # If a build or benchmark is still marked as "incomplete" at
            # reporting time, it must have been killed by a timeout,
            # out-of-memory (OOM), or some other uncaught exception
            if (
                key == fs.Keys.BUILD_STATUS or fs.Keys.TOOL_STATUS in key
            ) and value == build.FunctionStatus.INCOMPLETE:
                value = build.FunctionStatus.KILLED

            # Add stats ensuring that those are all in lower case
            evaluation_stats[key.lower()] = value

        self.all_stats.append(evaluation_stats)

    def sort_stats(self):
        """Sorts the stats list used the class sort_key key function"""
        self.all_stats.sort(key=self.sort_key)

    def __str__(self) -> str:
        """Returns table as a string"""
        #
        # Construct headers and column alignment lists
        #
        headers = []
        col_align = []

        # First headers
        first_columns = self.table_descriptor.get("first_columns", [])
        for column in first_columns:
            if not (self.lean and column.omit_if_lean):
                headers.append(column.column_header)
                col_align += (column.align,)

        # Per tool headers
        tool_columns = self.table_descriptor.get("tool_columns", {})
        tools = self.tools or []
        for tool in tools:

            # Don't duplicate columns if tool has an alternate tool listed
            if isinstance(tool_columns[tool], type):
                referenced_tool = tool_columns[tool]
                if referenced_tool in tools:
                    continue
                # Use the column specification of the referenced tool
                tool = referenced_tool

            for column in tool_columns[tool]:
                if not (self.lean and column.omit_if_lean):
                    headers.append(column.column_header)
                    col_align += (column.align,)

        # Final headers
        last_columns = self.table_descriptor.get("last_columns", [])
        for column in last_columns:
            if not (self.lean and column.omit_if_lean):
                headers.append(column.column_header)
                col_align += (column.align,)

        #
        # Construct table rows
        #
        rows = []
        last_row = None
        last_build_stats = None
        for build_stats in self.all_stats:
            row = []

            # First columns
            for entry in first_columns:
                entry_str = entry.get_str(build_stats, self.lean)
                if entry_str is not None:
                    row.append(entry_str)

            # Per tool columns
            for tool in tools:

                if not isinstance(tool_columns[tool], list):
                    referenced_tool = tool_columns[tool]
                    if referenced_tool in tools:
                        continue
                    tool = referenced_tool

                for entry in tool_columns[tool]:
                    entry_str = entry.get_str(build_stats, self.lean)
                    if entry_str is not None:
                        row.append(entry_str)

            # Final columns
            for entry in last_columns:
                entry_str = entry.get_str(build_stats, self.lean)
                if entry_str is not None:
                    row.append(entry_str)

            # See if this row should be merged with the last row
            if last_build_stats and self.merge_test_fn(last_build_stats, build_stats):
                # Merge with last row
                for col in range(0, len(first_columns)):
                    # If identical, don't duplicate
                    if last_row[col] != row[col]:
                        last_row[col] = _merge_join(last_row[col], row[col])
                for col in range(len(first_columns), len(row) - len(last_columns)):
                    # Allow duplicates
                    last_row[col] = _merge_join(last_row[col], row[col])
                for col in range(len(row) - len(last_columns), len(row)):
                    # If identical, don't duplicate
                    if last_row[col] != row[col]:
                        last_row[col] = _merge_join(last_row[col], row[col])
            else:
                rows.append(row)
                last_row = row
                last_build_stats = build_stats

        if not rows:
            rows = [["NO DATA"] + [" "] * (len(headers) - 1)]

        return tabulate(rows, headers=headers, tablefmt="grid", colalign=col_align)

    def create_csv_report(self):

        # Find all keys and use as column headers
        column_headers = set()
        for build_stats in self.all_stats:
            column_headers |= set(build_stats.keys())

        # Sort all columns alphabetically
        column_headers = sorted(column_headers)

        # Fill in blanks for each build
        report: List[Dict] = []
        for build_stats in self.all_stats:
            result = {k: "-" for k in column_headers}
            for k, v in build_stats.items():
                result[k] = v
            report.append(result)

        return report, column_headers

    @staticmethod
    def sort_key(build_stats: dict) -> Tuple:
        """Sort by timestamp.  If timestamp is missing, then list first."""
        return (
            build_stats[fs.Keys.TIMESTAMP].strftime("%Y%m%d%H:%M:%S")
            if fs.Keys.TIMESTAMP in build_stats
            else "-"
        )

    @staticmethod
    def get_report_name() -> str:
        return tkml_report.get_report_name()


################################################################################
# TABLE CLASS FOR BASIC LEMONADE REPORT
################################################################################


class LemonadeTable(Table):

    table_descriptor = {
        "first_columns": [
            TimestampStat("Timestamp", fs.Keys.TIMESTAMP, "%Y-%m-%d\n%H:%M:%S"),
            SimpleStat("Build Name", fs.Keys.BUILD_NAME, "s"),
            SimpleStat("Tools\nSequence", fs.Keys.SELECTED_SEQUENCE_OF_TOOLS, "s"),
            SimpleStat("Build\nStatus", fs.Keys.BUILD_STATUS, "s"),
        ],
    }


################################################################################
# TABLE CLASS FOR LEMONADE PERFORMANCE REPORT
################################################################################


class LemonadePerfTable(Table):

    table_descriptor = {
        "first_columns": [
            TimestampStat("Timestamp", fs.Keys.TIMESTAMP, "%Y-%m-%d\n%H:%M:%S"),
            # SimpleStat("Timestamp", fs.Keys.TIMESTAMP, "s"),
            MultiStat(
                "Model\n\nDevice\nData Type",
                [Keys.CHECKPOINT, None, Keys.DEVICE, Keys.DTYPE],
                "s",
                wrap=35,
            ),
        ],
        "tool_columns": {
            OgaBench: [
                SimpleStat(_wrap("Prompt Len (Tokens)", 8), Keys.PROMPT_TOKENS, "d"),
                StatWithSD(
                    _wrap("Time to First Token (sec)", 8),
                    Keys.SECONDS_TO_FIRST_TOKEN,
                    Keys.STD_DEV_SECONDS_TO_FIRST_TOKEN,
                    ".2f",
                ),
                StatWithSD(
                    _wrap("Tokens per Second", 8),
                    Keys.TOKEN_GENERATION_TOKENS_PER_SECOND,
                    Keys.STD_DEV_TOKENS_PER_SECOND,
                    ".2f",
                ),
                SimpleStat(
                    _wrap("Memory Used (GB)", 8), Keys.MAX_MEMORY_USED_GBYTE, ".3f"
                ),
            ],
            HuggingfaceBench: OgaBench,
            LlamaCppBench: OgaBench,
            AccuracyMMLU: [
                AdditionalStat(
                    "MMLU",
                    fs.Keys.AVERAGE_MMLU_ACCURACY + "|^mmlu_",
                    fs.Keys.AVERAGE_MMLU_ACCURACY,
                    ".2f",
                )
            ],
        },
        "last_columns": [
            SimpleStat(
                "System Info",
                fs.Keys.SYSTEM_INFO,
                "s",
                "left",
                omit_if_lean=True,
                wrap=50,
            ),
            SimpleStat(
                "Software Versions", SW_VERSIONS, "s", "left", omit_if_lean=True
            ),
        ],
    }

    basic_build_stats = [
        Keys.CHECKPOINT,
        Keys.DEVICE,
        Keys.DTYPE,
        fs.Keys.TIMESTAMP,
        fs.Keys.SYSTEM_INFO,
    ]

    def __init__(
        self,
        device: str = None,
        dtype: str = None,
        model: str = None,
        days: int = None,
        merge: bool = False,
        lean: bool = False,
    ):
        super().__init__()
        self.device = device
        if dtype:
            dtype = dtype.lower()
            dtype = dtype_aliases.get(dtype, dtype)
        self.dtype = dtype
        self.model = model
        self.days = days
        self.lean = lean
        self.tools_found_set = set()
        self.include_stats_filter = {
            Keys.DEVICE: self.device,
            Keys.CHECKPOINT: self.model,
        }
        if merge:
            self.merge_test_fn = LemonadePerfTable.matching_builds

    def load_stats(self):
        super().load_stats()
        self.tools = list(self.tools_found_set)
        self.tools.sort(key=lambda tool_class: tool_class.unique_name)

    def include_stats(self, model_stats) -> bool:
        """
        Returns True if the build was successful and matches
        the criteria specified, else returns False.
        """
        # Filter out builds that are incomplete
        if (
            not model_stats.get(fs.Keys.BUILD_STATUS, None)
            == build.FunctionStatus.SUCCESSFUL
        ):
            return False

        # Filter out build if it doesn't match specified dtype
        if self.dtype:
            build_dtype = model_stats.get(Keys.DTYPE, "").lower()
            build_dtype = dtype_aliases.get(build_dtype, build_dtype)
            if self.dtype not in build_dtype:
                return False

        # Filter out build if it doesn't match specified device or model
        for key, value in self.include_stats_filter.items():
            if value is not None:
                model_value = model_stats.get(key, "")
                if model_value is None or value.lower() not in model_value.lower():
                    return False

        # Filter out build if it is too old
        if not self.days is None:
            build_day = model_stats[fs.Keys.TIMESTAMP]
            today = datetime.now(timezone.utc)
            delta = today - build_day
            if delta.days > self.days:
                return False

        # All tests passed
        return True

    def post_process_stats(self, model_stats) -> bool:
        tool_columns = self.table_descriptor["tool_columns"]
        data = {}

        for tool in tool_columns.keys():
            tool_status_key = fs.Keys.TOOL_STATUS + ":" + tool.unique_name
            if (
                model_stats.get(tool_status_key, None)
                == build.FunctionStatus.SUCCESSFUL
            ):
                # Successful build of this tool, so remember this tool
                self.tools_found_set.add(tool)

                # Extract the declared tool stats
                data = data | {
                    stat: model_stats.get(stat, None) for stat in tool().status_stats
                }

                # Find if there are any additional stats for this tool
                # First see if this tool refers to another tool
                if not isinstance(tool_columns[tool], list):
                    tool = tool_columns[tool]
                regexp_list = [
                    stat.regexp
                    for stat in tool_columns[tool]
                    if isinstance(stat, AdditionalStat)
                ]
                match_expr = "(?:% s)" % "|".join(regexp_list)
                additional_stats = [
                    stat for stat in model_stats.keys() if re.match(match_expr, stat)
                ]
                data = data | {stat: model_stats[stat] for stat in additional_stats}

        if not data:
            # No matching tools successfully completed in this build
            return

        #
        # Add basic build stats
        #
        for key in self.basic_build_stats:
            data[key] = model_stats.get(key, "")

        # Create a new entry with Driver Versions and relevant Python Packages
        sw_versions = [
            key + ": " + value
            for key, value in data[fs.Keys.SYSTEM_INFO]["Driver Versions"].items()
        ]
        sw_versions += [
            pkg
            for pkg in data[fs.Keys.SYSTEM_INFO]["Python Packages"]
            if any(name in pkg for name in PYTHON_PACKAGES)
        ]
        data[SW_VERSIONS] = sw_versions

        # Exclude Python Packages and Driver Versions from System Info
        system_info = [
            key + ": " + str(value)
            for key, value in data[fs.Keys.SYSTEM_INFO].items()
            if key not in ["Python Packages", "Driver Versions"]
        ]
        data[fs.Keys.SYSTEM_INFO] = system_info

        self.all_stats.append(data)

    @staticmethod
    def sort_key(build_stats: dict) -> Tuple:
        return tuple(
            build_stats[key]
            for key in [
                Keys.CHECKPOINT,
                Keys.DEVICE,
                Keys.DTYPE,
                fs.Keys.SYSTEM_INFO,
                SW_VERSIONS,
                fs.Keys.TIMESTAMP,
            ]
        )

    @staticmethod
    def matching_builds(build_stats_1: Dict, build_stats_2: Dict) -> bool:
        """
        Returns true if the two builds have matching model, device, datatype,
        system info and SW versions
        """
        merge_key_list = [
            Keys.CHECKPOINT,
            Keys.DEVICE,
            Keys.DTYPE,
            fs.Keys.SYSTEM_INFO,
            SW_VERSIONS,
        ]
        dict_1 = {key: build_stats_1[key] for key in merge_key_list}
        dict_2 = {key: build_stats_2[key] for key in merge_key_list}

        return dict_1 == dict_2

    @staticmethod
    def get_report_name() -> str:
        current_time = datetime.now(timezone.utc)
        timestamp = current_time.strftime("%Y-%m-%d-%H%M%S")
        return f"{timestamp}_perf.csv"
