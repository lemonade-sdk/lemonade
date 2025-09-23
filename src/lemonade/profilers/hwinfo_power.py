#
# This power profiler uses an external tool called HWiNFO.
# Please see the power profiling documentation for download and install instructions.
#
# The power profiling functionality is currently not part of our continuous integration
# testing framework, primarily due to the setup overhead required from the above three items.
# We will revisit in the future if we face issues.
#

import ctypes
from datetime import datetime
import os
import platform
import textwrap
import time
import subprocess
import psutil
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import lemonade.common.printing as printing
from lemonade.profilers import Profiler
from lemonade.tools.report.table import LemonadePerfTable, DictListStat

DEFAULT_TRACK_POWER_INTERVAL_MS = 500
DEFAULT_TRACK_POWER_WARMUP_PERIOD = 5

HWINFO_PATH_ENV_VAR = "HWINFO_PATH"
DEFAULT_HWINFO_PATH = r"C:\Program Files\HWiNFO64\HWiNFO64.exe"
POWER_USAGE_CSV_FILENAME = "power_usage_hwinfo.csv"
POWER_USAGE_PNG_FILENAME = "power_usage_hwinfo.png"


class Keys:
    # Path to the file containing the power usage plot
    POWER_USAGE_PLOT = "power_usage_plot_hwinfo"
    # Path to the file containing the power usage plot
    POWER_USAGE_DATA = "power_usage_data_hwinfo"
    # Path to the file containing the power usage plot
    POWER_USAGE_DATA_CSV = "power_usage_data_file_hwinfo"
    # Maximum power consumed by the CPU processor package during the tools sequence
    PEAK_PROCESSOR_PACKAGE_POWER = "peak_processor_package_power_hwinfo"


# Add column to the Lemonade performance report table for the power data
LemonadePerfTable.table_descriptor["stat_columns"].append(
    DictListStat(
        "Power Usage (HWiNFO)",
        Keys.POWER_USAGE_DATA,
        [
            ("name", "{0}:"),
            ("duration", "{0:.1f}s,"),
            ("energy consumed", "{0:.1f} J"),
        ],
    )
)


def is_user_admin() -> bool:
    """Return true if platform is Windows and user is Admin"""
    os_type = platform.system()
    if os_type == "Windows":
        try:
            return ctypes.windll.shell32.IsUserAnAdmin() == 1
        except AttributeError:
            pass
    return False


def is_process_running(executable_name):
    """Checks if an executable is currently running."""
    executable_name = executable_name.lower()
    for process in psutil.process_iter(["pid", "name"]):
        if process.info["name"].lower() == executable_name:
            return True
    return False


def read_data_from_csv(csv_path, columns_dict, encoding="utf-8") -> pd.DataFrame:
    try:
        available_columns = pd.read_csv(csv_path, nrows=0, encoding=encoding).columns
        columns_to_read = list(set(columns_dict.values()) & set(available_columns))
        df = pd.read_csv(csv_path, usecols=columns_to_read, encoding=encoding)
    except FileNotFoundError as e:
        printing.log_info(f"Power profiler file not found: {e.filename}")
        return None
    except ValueError as e:
        printing.log_info(f"Error reading power data from {csv_path}: {e}")
        return None

    # Rename columns to simple name
    df.rename(
        columns={v: k for k, v in columns_dict.items() if v in columns_to_read},
        inplace=True,
    )

    return df


class HWINFOPowerProfiler(Profiler):

    unique_name = "power-hwinfo"

    # mapping from short name to full name of the measurement in the CSV file produced by HWiNFO
    columns_dict = {
        "time": "Time",
        "cpu_package_power": "CPU Package Power [W]",
        "npu_clock": "NPU Clock [MHz]",
        "gpu_clock": "GPU Clock [MHz]",
        "total_cpu_usage": "Total CPU Usage [%]",
        "apu_stapm_limit": "APU STAPM Limit [%]",
        "cpu_tdc_limit": "CPU TDC Limit [%]",
        "cpu_edc_limit": "CPU EDC Limit [%]",
        "cpu_ppt_fast_limit": "CPU PPT FAST Limit [%]",
        "cpu_ppt_slow_limit": "CPU PPT SLOW Limit [%]",
        "thermal_limit": "Thermal Limit [%]",
    }

    @staticmethod
    def time_to_seconds(time_str):
        # Parse the time string
        try:
            time_obj = datetime.strptime(time_str, "%H:%M:%S.%f")
        except TypeError:
            raise ValueError(f"Could not parse {time_str}")

        # Calculate the total seconds
        total_seconds = (
            time_obj.hour * 3600
            + time_obj.minute * 60
            + time_obj.second
            + time_obj.microsecond / 1_000_000
        )
        return total_seconds

    @staticmethod
    def add_arguments_to_parser(parser):
        parser.add_argument(
            f"--{HWINFOPowerProfiler.unique_name}",
            nargs="?",
            metavar="WARMUP_PERIOD",
            type=int,
            default=None,
            const=DEFAULT_TRACK_POWER_WARMUP_PERIOD,
            help="Track power consumption using the HWiNFO application and plot the results. "
            "HWiNFO is a commercial product from a third party (https://www.hwinfo.com/) "
            "and should be acquired/licensed appropriately. "
            "Optionally, set the warmup period in seconds "
            f"(default: {DEFAULT_TRACK_POWER_WARMUP_PERIOD}). If the application is not "
            f"installed at {DEFAULT_HWINFO_PATH}, set the {HWINFO_PATH_ENV_VAR} environment "
            f"variable to point at it. This is a Windows only feature and Lemonade must be run "
            f"from a CMD window with Administrator privileges.",
        )

    def __init__(self, parser_arg_value):
        super().__init__()
        self.warmup_period = parser_arg_value
        self.status_stats += [Keys.PEAK_PROCESSOR_PACKAGE_POWER, Keys.POWER_USAGE_PLOT]
        self.tracking_active = False
        self.build_dir = None
        self.csv_path = None
        self.hwinfo_process = None
        self.data = None

    def start(self, build_dir):
        if self.tracking_active:
            raise RuntimeError("Cannot start power tracking while already tracking")

        if platform.system() != "Windows":
            raise RuntimeError("Power usage tracking is only enabled in Windows.")

        # Check that user as running in Admin mode
        if not is_user_admin():
            raise RuntimeError(
                "For power usage tracking, run Lemonade as an Administrator."
            )

        # Save the folder where data and plot will be stored
        self.build_dir = build_dir

        # The csv file where power data will be stored
        self.csv_path = os.path.join(build_dir, POWER_USAGE_CSV_FILENAME)
        if " " in self.csv_path:
            raise RuntimeError(
                "Can't log HWiNFO data to a file with a <space> in the path. "
                "Please use the `-d` flag to specify a Lemonade cache path with no spaces."
            )

        # See if the HWINFO_PATH environment variables exists
        # If so,  use it instead of the default path
        if HWINFO_PATH_ENV_VAR in os.environ:
            hwinfo_path = os.getenv(HWINFO_PATH_ENV_VAR)
        else:
            hwinfo_path = DEFAULT_HWINFO_PATH

        # Check the HWINFO executable exists
        if not os.path.isfile(hwinfo_path):
            raise FileNotFoundError(hwinfo_path)

        # Check that executable is not already running
        executable = hwinfo_path.split(os.sep)[-1]
        if is_process_running(executable):
            raise RuntimeError(
                f"{executable} is already running.  Quit it and try again."
            )

        # Start HWiNFO executable
        try:
            command = [
                hwinfo_path,
                f"-l{self.csv_path}",
                f"-poll_rate={DEFAULT_TRACK_POWER_INTERVAL_MS}",
            ]
            self.hwinfo_process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError as e:
            if "[WinError 740]" in str(e):
                print(
                    "\nTo avoid `requested operation requires elevation` error, please make sure"
                )
                print(
                    "HWiNFO.exe has Properties->Compatibility->`Run this program as an "
                    "administrator` checked."
                )
                print(
                    "You may also need to set Windows User Account Control to `Never notify`.\n"
                )
            raise
        self.tracking_active = True
        time.sleep(self.warmup_period)

    def stop(self):
        if self.tracking_active:
            self.tracking_active = False
            time.sleep(self.warmup_period)
            self.hwinfo_process.terminate()
            self.hwinfo_process.wait()

    def generate_results(self, state, timestamp, start_times):
        if self.hwinfo_process is None:
            return

        if self.tracking_active:
            self.stop()

        df = read_data_from_csv(self.csv_path, self.columns_dict, encoding="latin1")
        if df is None:
            state.save_stat(Keys.POWER_USAGE_PLOT, "NONE")
            return

        # Remap time to seconds from start of profiling data
        # Remap csv data time to elapsed seconds (i.e., substract out initial time)
        try:
            initial_data_time = self.time_to_seconds(df["time"].iloc[0])
            df["time"] = df["time"].apply(
                lambda x: (self.time_to_seconds(x) - initial_data_time)
            )
        except ValueError as e:
            printing.log_info(
                f"Badly formatted time data in {self.csv_path}: {e}.  "
                f"HWiNFO may have closed unexpectedly."
            )
            state.save_stat(Keys.POWER_USAGE_PLOT, "NONE")
            return

        # Make time 0 the time of the first tool starting (after the warmup period)
        if start_times:
            tool_start_times = sorted(start_times.values())
            # First tool after warmup (if no tools, then will be time of start of cool down)
            first_tool_time = tool_start_times[1]

            # Map the measurement data so that zero in the measurement data aligns with
            # the first_tool_time
            #
            # Find the difference between the timestamp first_tool_time and initial_data_time
            # which is a count of seconds since midnight
            #
            # Find midnight prior to first_tool_time
            t = time.localtime(first_tool_time)
            since_midnight = (
                t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec + (first_tool_time % 1)
            )
            delta = since_midnight - initial_data_time
            df["time"] = df["time"] - delta

        peak_power = max(df["cpu_package_power"])

        # Create a figure
        fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(16, 8))

        if start_times:
            tool_starts = sorted(start_times.items(), key=lambda item: item[1])
            tool_name_list = [item[0] for item in tool_starts]

            # Adjust to common time frame as power measurements
            tool_start_list = [
                max(df["time"].iloc[0], item[1] - first_tool_time)
                for item in tool_starts
            ]
            tool_stop_list = tool_start_list[1:] + [df["time"].values[-1]]

            # Extract power data time series
            x_time = df["time"].to_numpy()
            y_power = df["cpu_package_power"].to_numpy()

            # Extract data for each stage in the build
            self.data = []
            for name, t0, tf in zip(tool_name_list, tool_start_list, tool_stop_list):
                x = x_time[(x_time >= t0) * (x_time <= tf)]
                x = np.insert(x, 0, t0)
                x = np.insert(x, len(x), tf)
                y = np.interp(x, x_time, y_power)
                energy = np.trapz(y, x)
                avg_power = energy / (tf - t0)
                stage = {
                    "name": name,
                    "t": x.tolist(),
                    "power": y.tolist(),
                    "duration": float(tf - t0),
                    "energy consumed": float(energy),
                    "average power": float(avg_power),
                }
                self.data.append(stage)

            for stage in self.data:
                # Plot power usage time series
                p = ax1.plot(
                    stage["t"],
                    stage["power"],
                    label=f"{stage['name']} ({stage['duration']:.1f}s, "
                    f"{stage['energy consumed']:0.1f} J)",
                )
                # Add a dashed line to show average power
                ax1.plot(
                    [stage["t"][0], stage["t"][-1]],
                    [stage["average power"], stage["average power"]],
                    linestyle="--",
                    c=p[0].get_c(),
                )
                # Add average power text to plot
                ax1.text(
                    stage["t"][0],
                    stage["average power"],
                    f"{stage['average power']:.1f} W ",
                    horizontalalignment="right",
                    verticalalignment="center",
                    c=p[0].get_c(),
                )
        else:
            ax1.plot(
                df["time"],
                df["cpu_package_power"],
            )
        # Add title and labels to plots
        ax1.set_ylabel(self.columns_dict["cpu_package_power"])
        title_str = "HWiNFO Stats\n" + "\n".join(textwrap.wrap(state.build_name, 60))
        ax1.set_title(title_str)
        ax1.legend()
        ax1.grid(True)

        # Create second plot
        ax2.plot(
            df["time"],
            df["npu_clock"],
            label=self.columns_dict["npu_clock"],
        )
        ax2.plot(
            df["time"],
            df["gpu_clock"],
            label=self.columns_dict["gpu_clock"],
        )
        ax2.set_xlabel("Time [s]")
        ax2.set_ylabel("Clock Frequency [MHz]")
        ax2.legend(loc=2)
        ax2.grid(True)
        # Add second y-axis for %
        ax2_twin = ax2.twinx()
        ax2_twin.plot(
            df["time"],
            df["total_cpu_usage"],
            label=self.columns_dict["total_cpu_usage"],
            c="g",
        )
        ax2_twin.set_ylim([0, 100])
        vals = ax2_twin.get_yticks()
        ax2_twin.set_yticks(vals)
        ax2_twin.set_yticklabels([f"{v:.0f}%" for v in vals])
        ax2_twin.legend(loc=1)

        # Create third plot (all remaining columns)
        plot3_columns = [
            "apu_stapm_limit",
            "cpu_tdc_limit",
            "cpu_edc_limit",
            "cpu_ppt_fast_limit",
            "cpu_ppt_slow_limit",
            "thermal_limit",
        ]
        for col_str in plot3_columns:
            if col_str in df.columns:
                ax3.plot(
                    df["time"],
                    df[col_str],
                    label=self.columns_dict[col_str],
                )
        ax3.set_xlabel("Time [s]")
        ax3.set_ylim([0, 100])
        vals = ax3.get_yticks()
        ax3.set_yticks(vals)
        ax3.set_yticklabels([f"{v:.0f}%" for v in vals])
        if len(ax3.lines):
            ax3.legend()
        ax3.grid(True)

        # Save plot to current folder AND save to cache
        plot_path = os.path.join(
            self.build_dir, f"{timestamp}_{POWER_USAGE_PNG_FILENAME}"
        )
        fig.savefig(plot_path, dpi=300, bbox_inches="tight")
        plot_path = os.path.join(os.getcwd(), f"{timestamp}_{POWER_USAGE_PNG_FILENAME}")
        fig.savefig(plot_path, dpi=300, bbox_inches="tight")

        state.save_stat(Keys.POWER_USAGE_PLOT, plot_path)
        state.save_stat(Keys.POWER_USAGE_DATA, self.data)
        state.save_stat(Keys.POWER_USAGE_DATA_CSV, self.csv_path)
        state.save_stat(Keys.PEAK_PROCESSOR_PACKAGE_POWER, f"{peak_power:0.1f} W")
