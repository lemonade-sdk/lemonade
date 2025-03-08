import argparse
import csv
import os
from pathlib import Path
import re
from typing import List
import turnkeyml.common.printing as printing
import turnkeyml.common.filesystem as fs
from turnkeyml.tools.management_tools import ManagementTool
from lemonade.cache import DEFAULT_CACHE_DIR
from lemonade.tools.report.table import LemonadeTable, LemonadePerfTable


class LemonadeReport(ManagementTool):
    """
    Analyzes the input lemonade cache(s) and produces an aggregated report
    in csv format that contains the build stats for all builds in all cache(s).
    (Identical to the turnkeyml report tool, except default input cache and output folders.)

    In addition, summary information is printed to the console and saved to a text file.

    When the --perf flag is used, then a performance report is generated that summarizes
    the performance data for different models.  In this case, only the data used in the
    text table is saved to the csv format file.
    """

    unique_name = "report"

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Export statistics from each lemonade run to a CSV file",
            add_help=add_help,
        )

        parser.prog = parser.prog.replace("turnkey", "lemonade")

        parser.add_argument(
            "-i",
            "--input-caches",
            nargs="*",
            default=[DEFAULT_CACHE_DIR],
            help=(
                "One or more lemonade cache directories to use to generate the report "
                f"(defaults to {DEFAULT_CACHE_DIR})"
            ),
        )

        parser.add_argument(
            "-o",
            "--output-dir",
            help="Path to folder where reports will be saved "
            "(defaults to current working directory)",
            required=False,
            default=os.getcwd(),
        )

        parser.add_argument(
            "--no-save",
            action="store_true",
            help="Don't save output to TXT and CSV files",
        )

        parser.add_argument(
            "--perf",
            action="store_true",
            help="Produce the performance table instead of the regular table",
        )

        parser.add_argument(
            "--device",
            default=None,
            help="In the --perf table, only include output for the specified device "
            "(e.g., cpu, igpu, npu, hybrid)",
        )

        parser.add_argument(
            "--dtype",
            default=None,
            help="In the --perf table, only include output for the specified datatype "
            "(e.g., float32, int4)",
        )

        parser.add_argument(
            "--model",
            default=None,
            help="In the --perf table, only include output for builds with name that contains"
            " the specified string (e.g., Llama).  The string match is case-insensitive.",
        )

        parser.add_argument(
            "--days",
            "-d",
            type=int,
            metavar="N",
            default=None,
            help="In the --perf table, only include output for builds from the last N days.",
        )

        parser.add_argument(
            "--merge",
            action="store_true",
            help="In the --perf table, merge results from different builds into the same row "
            "as long as model, device, datatype, system info and package version are the same.",
        )

        parser.add_argument(
            "--lean",
            action="store_true",
            help="In the --perf table, don't include the system info and sw package versions.",
        )

        return parser

    def parse(self, args, known_only=True) -> argparse.Namespace:
        """
        Helper function to parse CLI arguments into the args expected by run()
        """
        parsed_args = super().parse(args, known_only)

        if not parsed_args.perf:
            # Check that none of the perf specific flags are set
            perf_args = [
                parsed_args.device,
                parsed_args.dtype,
                parsed_args.days,
                parsed_args.merge,
                parsed_args.lean,
            ]
            if not all(arg is None or arg is False for arg in perf_args):
                raise ValueError(
                    "Invalid arguments for regular report.  Did you miss the --perf argument?"
                    "  See `lemonade report -h` for help."
                )

        return parsed_args

    def run(
        self,
        _,
        input_caches: List[str] = None,
        output_dir: str = os.getcwd(),
        no_save: bool = False,
        perf: bool = False,
        device: str = None,
        dtype: str = None,
        model: str = None,
        days: int = None,
        merge: bool = False,
        lean: bool = False,
    ):
        # Process input arguments
        cache_dirs = [os.path.expanduser(dir) for dir in input_caches]
        cache_dirs = fs.expand_inputs(cache_dirs)
        report_dir = os.path.expanduser(output_dir)

        if perf:
            table = LemonadePerfTable(device, dtype, model, days, merge, lean)
        else:
            table = LemonadeTable()

        # Find builds and load stats
        table.find_builds(cache_dirs, model)
        table.load_stats()
        table.sort_stats()

        # Print message if there are no stats
        if len(table.all_stats) == 0:
            printing.log_info("No relevant cached build data found")
            return

        # Print table to stdout
        print()
        print(table)

        if no_save:
            return

        # Name report file
        report_path = os.path.join(report_dir, table.get_report_name())
        txt_path = re.sub(".csv$", ".txt", report_path)
        Path(report_dir).mkdir(parents=True, exist_ok=True)

        # Create the report to save to CSV
        report, column_headers = table.create_csv_report()

        # Populate results spreadsheet
        with open(report_path, "w", newline="", encoding="utf8") as spreadsheet:
            writer = csv.writer(spreadsheet)
            writer.writerow(column_headers)
            for entry in report:
                writer.writerow([entry[col] for col in column_headers])

        # Save the text report
        with open(txt_path, "w", encoding="utf-8") as file:
            print(table, file=file)

        # Print message with the output file path
        printing.log("Report text saved at ")
        printing.logn(str(txt_path), printing.Colors.OKGREEN)
        printing.log("Report spreadsheet saved at ")
        printing.logn(str(report_path), printing.Colors.OKGREEN)
