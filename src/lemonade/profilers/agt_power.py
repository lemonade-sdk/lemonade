#
# This power profiler uses a tool that is not publicly available yet.
# Please see the power profiling documentation for download and install instructions.
#
# The power profiling functionality is currently not part of our continuous integration
# testing framework, primarily due to the setup overhead required from the above three items.
# We will revisit in the future if we face issues.
#

from datetime import datetime
import os
import platform
import textwrap
import time
import re
import subprocess
import matplotlib.pyplot as plt
import numpy as np
import lemonade.common.printing as printing
from lemonade.profilers import Profiler
from lemonade.tools.report.table import LemonadePerfTable, DictListStat
from lemonade.profilers.hwinfo_power import (
    is_user_admin,
    is_process_running,
    read_data_from_csv,
)

AGT_PATH_ENV_VAR = "AGT_PATH"
DEFAULT_TRACK_POWER_INTERVAL_MS = 500
DEFAULT_TRACK_POWER_WARMUP_PERIOD = 5
POWER_USAGE_CSV_FILENAME = "power_usage_agt.csv"
POWER_USAGE_PNG_FILENAME = "power_usage_agt.png"


class Keys:
    # Path to the file containing the power usage plot
    POWER_USAGE_PLOT = "power_usage_plot_agt"
    # Path to the file containing the power usage plot
    POWER_USAGE_DATA = "power_usage_data_agt"
    # Path to the file containing the power usage plot
    POWER_USAGE_DATA_CSV = "power_usage_data_file_agt"
    # Maximum power consumed by the APU processor package during the tools sequence
    PEAK_PROCESSOR_PACKAGE_POWER = "peak_processor_package_power_agt"


# Add column to the Lemonade performance report table for the power data
LemonadePerfTable.table_descriptor["stat_columns"].append(
    DictListStat(
        "Power Usage (AGT)",
        Keys.POWER_USAGE_DATA,
        [
            ("name", "{0}:"),
            ("duration", "{0:.1f}s,"),
            ("energy consumed", "{0:.1f} J"),
        ],
    )
)


class AGTPowerProfiler(Profiler):

    unique_name = "power-agt"

    # mapping from short name to full name of the measurement in the CSV file produced by AGT
    columns_dict = {
        "time": "Time Stamp",
        "cpu_package_power": "CPU0 Power Correlation SOCKET Power ",
        "npu_clock": "CPU0 Frequencies Actual Frequency NPUHCLK",
        "gpu_clock": "CPU0 GFX GFX Freq Eff",
        # for processors with classic and dense cores
        "classic_cpu_usage": "CPU0 DPM Activity Monitors Busy Value CLASSIC_C0",
        "dense_cpu_usage": "CPU0 DPM Activity Monitors Busy Value DENSE_C0",
        # for multi CCD processors
        "classic_cpu_usage_0": "CPU0 DPM Activity Monitors Busy Value CCD0_C0",
        "classic_cpu_usage_1": "CPU0 DPM Activity Monitors Busy Value CCD1_C0",
        #
        "apu_stapm_value": "CPU0 INFRASTRUCTURE1 Value STAPM",
        "apu_stapm_limit": "CPU0 INFRASTRUCTURE1 Limit STAPM",
        "cpu_tdc_value": "CPU0 INFRASTRUCTURE1 Value TDC VDD",
        "cpu_tdc_limit": "CPU0 INFRASTRUCTURE1 Limit TDC VDD",
        "cpu_edc_value": "CPU0 EDC VDD Peak Telemetry Current ",
        "cpu_edc_limit": "CPU0 EDC Effective VDD EDC Limit ",
        "cpu_ppt_fast_value": "CPU0 INFRASTRUCTURE1 Value PPT FAST",
        "cpu_ppt_fast_limit": "CPU0 INFRASTRUCTURE1 Limit PPT FAST",
        "cpu_ppt_slow_value": "CPU0 INFRASTRUCTURE1 Value PPT SLOW",
        "cpu_ppt_slow_limit": "CPU0 INFRASTRUCTURE1 Limit PPT SLOW",
        "cpu_thermal_value_ccx0": "CPU0 INFRASTRUCTURE2 Value THM CCX0",
        "cpu_thermal_limit_ccx0": "CPU0 INFRASTRUCTURE2 Limit THM CCX0",
        "cpu_thermal_value_ccx1": "CPU0 INFRASTRUCTURE2 Value THM CCX1",
        "cpu_thermal_limit_ccx1": "CPU0 INFRASTRUCTURE2 Limit THM CCX1",
        "cpu_thermal_value_gfx": "CPU0 INFRASTRUCTURE2 Value THM GFX",
        "cpu_thermal_limit_gfx": "CPU0 INFRASTRUCTURE2 Limit THM GFX",
        "cpu_thermal_value_soc": "CPU0 INFRASTRUCTURE2 Value THM SOC",
        "cpu_thermal_limit_soc": "CPU0 INFRASTRUCTURE2 Limit THM SOC",
        "cpu_stt_value": "CPU0 INFRASTRUCTURE2 Value STT APU",
        "cpu_stt_limit": "CPU0 INFRASTRUCTURE2 Limit STT APU",
    }

    @staticmethod
    def time_to_seconds(time_str):

        # Parse the time string
        match = re.search(r"\b(\d{2}:\d{2}:\d{2}.\d{3})", time_str)
        if match:
            time_obj = datetime.strptime(match.group(), "%H:%M:%S.%f")
        else:
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
            f"--{AGTPowerProfiler.unique_name}",
            nargs="?",
            metavar="WARMUP_PERIOD",
            type=int,
            default=None,
            const=DEFAULT_TRACK_POWER_WARMUP_PERIOD,
            help="Track power consumption using the AGT application and plot the results. "
            "AGT is an internal AMD tool. "
            "Optionally, set the warmup period in seconds "
            f"(default: {DEFAULT_TRACK_POWER_WARMUP_PERIOD}). "
            f"Set the {AGT_PATH_ENV_VAR} environment variable to point to the AGT executable. "
            "This is a Windows only feature and Lemonade must be run from a CMD "
            "window with Administrator privileges.",
        )

    def __init__(self, parser_arg_value):
        super().__init__()
        self.warmup_period = parser_arg_value
        self.status_stats += [Keys.PEAK_PROCESSOR_PACKAGE_POWER, Keys.POWER_USAGE_PLOT]
        self.tracking_active = False
        self.build_dir = None
        self.csv_path = None
        self.agt_process = None
        self.data = None

    def start(self, build_dir):
        if self.tracking_active:
            raise RuntimeError("Cannot start power tracking while already tracking")

        if platform.system() != "Windows":
            raise RuntimeError("Power usage tracking is only enabled in Windows.")

        # Check that user is running in Admin mode
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
                "Can't log AGT data to a file with a <space> in the path. "
                "Please use the `-d` flag to specify a Lemonade cache path with no spaces."
            )

        # Check the AGT environment variables exists
        if AGT_PATH_ENV_VAR in os.environ:
            agt_path = os.getenv(AGT_PATH_ENV_VAR)
        else:
            raise RuntimeError(
                f"Set environment variable {AGT_PATH_ENV_VAR} to point to the AGT executable."
            )

        # Check the AGT executable exists
        if not os.path.isfile(agt_path):
            raise FileNotFoundError(agt_path)

        # Check that executable is not already running
        executable = agt_path.split(os.sep)[-1]
        if is_process_running(executable):
            raise RuntimeError(
                f"{executable} is already running.  Quit it and try again."
            )

        # Start AGT executable
        try:
            command = [
                agt_path,
                "-i=1",
                "-unilog=PM",
                "-unilogallgroups",
                f"-unilogperiod={DEFAULT_TRACK_POWER_INTERVAL_MS}",
                f"-unilogoutput={self.csv_path}",
            ]
            self.agt_process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError as e:
            if "[WinError 740]" in str(e):
                print(
                    "You may need to set Windows User Account Control to `Never notify`.\n"
                )
            raise
        self.tracking_active = True
        time.sleep(self.warmup_period)

    def stop(self):
        if self.tracking_active:
            self.tracking_active = False
            time.sleep(self.warmup_period)
            self.agt_process.terminate()
            self.agt_process.wait()

    def generate_results(self, state, timestamp, start_times):
        if self.agt_process is None:
            return

        if self.tracking_active:
            self.stop()

        df = read_data_from_csv(self.csv_path, self.columns_dict)
        if df is None:
            state.save_stat(Keys.POWER_USAGE_PLOT, "NONE")
            return

        # Remap csv data time to elapsed seconds (i.e., substract out initial time)
        try:
            initial_data_time = self.time_to_seconds(df["time"].iloc[0])
            df["time"] = df["time"].apply(
                lambda x: (self.time_to_seconds(x) - initial_data_time)
            )
        except ValueError as e:
            printing.log_info(f"Badly formatted time data in {self.csv_path}: {e}.")
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

        # Scale value metrics with limits to percentages
        for col_name in df.columns:
            if "value" in col_name:
                limit_col_name = col_name.replace("value", "limit")
                if limit_col_name in df.columns:
                    df[col_name] = 100 * df[col_name] / df[limit_col_name]

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
        title_str = "AGT Stats\n" + "\n".join(textwrap.wrap(state.build_name, 60))
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
        # Add second y-axis for percentage metrics
        # Manually set colors to be different from first axis
        ax2_twin = ax2.twinx()
        if "classic_cpu_usage" in df.columns:
            ax2_twin.plot(
                df["time"],
                df["classic_cpu_usage"],
                label=self.columns_dict["classic_cpu_usage"],
                c="g",
            )
        if "dense_cpu_usage" in df.columns:
            ax2_twin.plot(
                df["time"],
                df["dense_cpu_usage"],
                label=self.columns_dict["dense_cpu_usage"],
                c="r",
            )
        if "classic_cpu_usage_0" in df.columns:
            ax2_twin.plot(
                df["time"],
                df["classic_cpu_usage_0"],
                label=self.columns_dict["classic_cpu_usage_0"],
                c="g",
            )
        if "classic_cpu_usage_1" in df.columns:
            ax2_twin.plot(
                df["time"],
                df["classic_cpu_usage_1"],
                label=self.columns_dict["classic_cpu_usage_1"],
                c="r",
            )
        ax2_twin.set_ylim([0, 100])
        vals = ax2_twin.get_yticks()
        ax2_twin.set_yticks(vals)
        ax2_twin.set_yticklabels([f"{v:.0f}%" for v in vals])
        ax2_twin.legend(loc=1)

        # Create third plot (all remaining columns)
        plot3_columns = [
            "apu_stapm_value",
            "cpu_tdc_value",
            "cpu_edc_value",
            "cpu_ppt_fast_value",
            "cpu_ppt_slow_value",
            "cpu_thermal_value_ccx0",
            "cpu_thermal_value_ccx1",
            "cpu_thermal_value_gfx",
            "cpu_thermal_value_soc",
            "cpu_stt_value",
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
