import os
import argparse
import csv
from datetime import datetime
from pathlib import Path
from typing import Dict, List
import yaml
import pandas as pd
import turnkeyml.common.printing as printing
import turnkeyml.common.filesystem as fs
import turnkeyml.common.build as build
from turnkeyml.tools.management_tools import ManagementTool


def get_report_name(prefix: str = "") -> str:
    """
    Returns the name of the .csv report
    """
    day = datetime.now().day
    month = datetime.now().month
    year = datetime.now().year
    date_key = f"{year}-{str(month).zfill(2)}-{str(day).zfill(2)}"
    return f"{prefix}{date_key}.csv"


def _good_get(
    dict: Dict, key: str, return_keys: bool = False, return_values: bool = False
):
    if key in dict:
        if return_keys:
            return list(dict[key].keys())
        elif return_values:
            return list(dict[key].values())
        else:
            return dict[key]
    else:
        return "-"


class Report(ManagementTool):
    """
    Analyzes the input turnkeyml cache(s) and produces an aggregated report
    in csv format that contains the build stats for all builds in all cache(s).
    """

    unique_name = "report"

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Export statistics from each turnkey run to a CSV file",
            add_help=add_help,
        )

        parser.add_argument(
            "-i",
            "--input-caches",
            nargs="*",
            default=[fs.DEFAULT_CACHE_DIR],
            help=(
                "One or more turnkey cache directories to use to generate the report "
                f"(defaults to {fs.DEFAULT_CACHE_DIR})"
            ),
        )

        parser.add_argument(
            "-o",
            "--output-dir",
            help="Path to folder where report will be saved "
            "(defaults to current working directory)",
            required=False,
            default=os.getcwd(),
        )

        return parser

    def run(
        self,
        _,
        input_caches: List[str] = None,
        output_dir: str = os.getcwd(),
    ):
        # Input arguments from CLI
        cache_dirs = [os.path.expanduser(dir) for dir in input_caches]
        cache_dirs = fs.expand_inputs(cache_dirs)
        report_dir = os.path.expanduser(output_dir)

        # Name report file
        report_path = os.path.join(report_dir, get_report_name())

        # Create report dict
        Path(report_dir).mkdir(parents=True, exist_ok=True)

        report: List[Dict] = []
        all_evaluation_stats = []

        # Add results from all user-provided cache folders
        for cache_dir in cache_dirs:
            # Check if this is a valid cache directory
            fs.check_cache_dir(cache_dir)

            # List all yaml files available
            all_model_stats_yamls = fs.get_all(
                path=cache_dir, file_type="turnkey_stats.yaml"
            )
            all_model_stats_yamls = sorted(all_model_stats_yamls)

            # Bring all of the stats for all of the models into memory
            for model_stats_yaml in all_model_stats_yamls:
                with open(model_stats_yaml, "r", encoding="utf8") as stream:
                    try:
                        # load the yaml into a dict
                        model_stats = yaml.load(stream, Loader=yaml.FullLoader)

                        # Copy the stats to a new dictionary, making any necessary modifications
                        # along the way
                        evaluation_stats = {}

                        for key, value in model_stats.items():
                            # If a build or benchmark is still marked as "incomplete" at
                            # reporting time, it must have been killed by a time out,
                            # out-of-memory (OOM), or some other uncaught exception
                            if (
                                key == fs.Keys.BUILD_STATUS
                                or fs.Keys.TOOL_STATUS in key
                            ) and value == build.FunctionStatus.INCOMPLETE:
                                value = build.FunctionStatus.KILLED

                            # Add stats ensuring that those are all in lower case
                            evaluation_stats[key.lower()] = value

                        all_evaluation_stats.append(evaluation_stats)
                    except yaml.scanner.ScannerError:
                        continue

        # Scan the build stats to determine the set of columns for the CSV file.
        # The CSV will have one column for every key in any build stats dict.
        column_headers = []
        for evaluation_stats in all_evaluation_stats:
            # Add any key that isn't already in column_headers
            for header in evaluation_stats.keys():
                if header not in column_headers:
                    column_headers.append(header)

        # Sort all columns alphabetically
        column_headers = sorted(column_headers)

        # Add each build to the report
        for evaluation_stats in all_evaluation_stats:
            # Start with a dictionary where all of the values are "-". If a build
            # has a value for each key we will fill it in, and otherwise the "-"
            # will indicate that no value was available
            result = {k: "-" for k in column_headers}

            for key in column_headers:
                result[key] = _good_get(evaluation_stats, key)

            report.append(result)

        # Populate results spreadsheet
        with open(report_path, "w", newline="", encoding="utf8") as spreadsheet:
            writer = csv.writer(spreadsheet)
            writer.writerow(column_headers)
            for entry in report:
                writer.writerow([entry[col] for col in column_headers])

        # Print message with the output file path
        printing.log("Summary spreadsheet saved at ")
        printing.logn(str(report_path), printing.Colors.OKGREEN)

        # Save the unique errors and counts to a file
        errors = []
        for evaluation_stats in all_evaluation_stats:
            if (
                "compilation_error" in evaluation_stats.keys()
                and "compilation_error_id" in evaluation_stats.keys()
            ):
                error = evaluation_stats["compilation_error"]
                id = evaluation_stats["compilation_error_id"]
                if id != "":
                    unique_error = True
                    for reported_error in errors:
                        if reported_error["id"] == id:
                            unique_error = False
                            reported_error["count"] = reported_error["count"] + 1
                            reported_error["models_impacted"] = reported_error[
                                "models_impacted"
                            ] + [evaluation_stats["model_name"]]

                    if unique_error:
                        reported_error = {
                            "id": id,
                            "count": 1,
                            "models_impacted": [evaluation_stats["model_name"]],
                            "example": error,
                        }
                        errors.append(reported_error)

        if len(errors) > 0:
            errors_path = os.path.join(report_dir, get_report_name("errors-"))
            with open(errors_path, "w", newline="", encoding="utf8") as spreadsheet:
                writer = csv.writer(spreadsheet)
                error_headers = errors[0].keys()
                writer.writerow(error_headers)
                for unique_error in errors:
                    writer.writerow([unique_error[col] for col in error_headers])

            printing.log("Compilation errors spreadsheet saved at ")
            printing.logn(str(errors_path), printing.Colors.OKGREEN)
        else:
            printing.logn(
                "No compilation errors in any cached build, skipping errors spreadsheet."
            )


def get_dict(report_csv: str, columns: List[str]) -> Dict[str, Dict[str, str]]:
    """
    Returns a dictionary where the keys are model names and the values are dictionaries.
    Each dictionary represents a model with column names as keys and their corresponding values.
    args:
     - report_csv: path to a report.csv file generated by turnkey CLI
     - columns: list of column names in the report.csv file whose values will be used to
        populate the dictionary
    """

    # Load the report as a dataframe
    dataframe = pd.read_csv(report_csv)

    # Create a nested dictionary with model_name as keys and another
    # dictionary of {column: value} pairs as values
    result = {
        row[0]: row[1].to_dict()
        for row in dataframe.set_index("model_name")[columns].iterrows()
    }

    return result
