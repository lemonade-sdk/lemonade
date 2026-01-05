"""
FLM (FastFlowLM) utilities for installation, version checking, and model management.
"""

import os
import importlib.resources
import subprocess
import tempfile
import threading
import time
from typing import List, Optional
from openai import OpenAI

import requests
from packaging.version import Version, InvalidVersion
from lemonade.tools.adapter import PassthroughTokenizer, ModelAdapter
from lemonade.tools.server.utils.port import find_free_port
from lemonade.tools.llamacpp.utils import monitor_process_memory
import lemonade.common.printing as printing


def get_flm_latest_version() -> Optional[str]:
    """
    Get and return the latest FLM version from "https://github.com/FastFlowLM/FastFlowLM/tags"
    This uses the GitHub tags API.
    """
    url = "https://api.github.com/repos/FastFlowLM/FastFlowLM/tags"
    try:
        response = requests.get(url, timeout=10)
        response.raise_for_status()
        tags = response.json()
        if not tags:
            return None
        # Tags are sorted in reverse chronological order; find the first that looks like a version
        for tag in tags:
            tag_name = tag.get("name", "")
            # Accept tags of the form v0.9.10, 0.9.10, etc.
            if tag_name.startswith("v"):
                version_candidate = tag_name[1:]
            else:
                version_candidate = tag_name
            try:
                # validate it's a version string
                _ = Version(version_candidate)
                return version_candidate
            except InvalidVersion:
                continue
        return None
    except requests.exceptions.RequestException as e:
        printing.log_warning(f"Error retrieving latest FLM version: {e}")
        return None


def get_flm_required_version() -> str:
    """
    Return the required FLM version from the backend_versions.json file.
    """
    with importlib.resources.open_text("lemonade", "backend_versions.json") as f:
        import json

        versions_data = json.load(f)
        flm = versions_data.get("flm", None)
        if flm:
            flm_version = flm.get("version", None)
            if flm_version:
                return flm_version[1:]  # strip leading 'v'
    raise RuntimeError("Required FLM version not specified in backend_versions.json")


def get_flm_minimum_npu_driver() -> str:
    """
    Return the minimum NPU driver required by the FLM backend from the backend_versions.json file.
    """
    with importlib.resources.open_text("lemonade", "backend_versions.json") as f:
        import json

        versions_data = json.load(f)
        flm = versions_data.get("flm", None)
        if flm:
            min_npu_driver = flm.get("min_npu_driver", None)
            if min_npu_driver:
                return min_npu_driver
    raise RuntimeError(
        "Minimum NPU driver for FLM server not specified in backend_versions.json"
    )


def check_flm_version() -> Optional[str]:
    """
    Check if FLM is installed and return version, or None if not available.
    """
    required_version_str = get_flm_required_version()
    try:
        result = subprocess.run(
            ["flm", "version"],
            capture_output=True,
            text=True,
            check=True,
            encoding="utf-8",
            errors="replace",
        )

        # Parse version from output like "FLM v0.9.4"
        output = result.stdout.strip()
        if output.startswith("FLM v"):
            version_str = output[5:]  # Remove "FLM v" prefix
            return version_str, required_version_str
        return None, required_version_str

    except (subprocess.CalledProcessError, FileNotFoundError):
        return None, required_version_str


def get_npu_driver_version() -> Optional[str]:
    """
    Get the installed NPU driver version.
    Returns the version string or None if not available.
    """
    # Check if Windows OS
    if os.name != "nt":
        return "0.0.0.0"  # NPU driver check is only for Windows

    # Use WMI to query NPU driver version
    from lemonade.common.system_info import WindowsSystemInfo

    system_info = WindowsSystemInfo()
    npu_driver_version = system_info.get_driver_version(
        "NPU Compute Accelerator Device"
    )
    return npu_driver_version


def check_npu_driver_version():
    """
    Check the installed NPU driver version using 'npu-smi info' command.
    Returns the version string or None if not available.
    """
    npu_driver_version = get_npu_driver_version()
    minimum_npu_driver_version = get_flm_minimum_npu_driver()
    if npu_driver_version == "0.0.0.0":
        printing.log_warning("NPU driver check not available.")
    printing.log_info(f"Current NPU driver version: {npu_driver_version}")
    printing.log_info(
        f"Minimum required NPU driver version for FLM: {minimum_npu_driver_version}"
    )
    if Version(npu_driver_version) < Version(minimum_npu_driver_version):
        printing.log_warning(
            f"NPU driver version {npu_driver_version} is below the minimum required "
            f"version {minimum_npu_driver_version} for FLM.  Please upgrade your NPU driver"
        )
    else:
        printing.log_info("NPU driver version meets the minimum requirement for FLM.")


def refresh_environment():
    """
    Refresh PATH to pick up newly installed executables.
    """
    if os.name == "nt":  # Windows
        # On Windows, we need to refresh the PATH from registry
        import winreg

        try:
            with winreg.OpenKey(
                winreg.HKEY_LOCAL_MACHINE,
                r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment",
            ) as key:
                path_value, _ = winreg.QueryValueEx(key, "PATH")
                os.environ["PATH"] = path_value + ";" + os.environ.get("PATH", "")
        except Exception as e:  # pylint: disable=broad-except
            printing.log_warning(f"Could not refresh PATH from registry: {e}")

        # Also try to add common installation paths
        common_paths = [
            r"C:\Program Files\FLM",
            r"C:\Program Files (x86)\FLM",
            os.path.expanduser(r"~\AppData\Local\FLM"),
        ]
        for path in common_paths:
            if os.path.exists(path) and path not in os.environ.get("PATH", ""):
                os.environ["PATH"] = path + ";" + os.environ.get("PATH", "")


def install_flm():
    """
    Check if FLM is installed and at minimum version.
    If not, download and run the GUI installer, then wait for completion.
    """
    # Check NPU driver version
    check_npu_driver_version()

    # Check current FLM installation
    current_version, required_version = check_flm_version()

    if current_version and Version(current_version) == Version(required_version):
        printing.log_info(
            f"FLM v{current_version} is already installed and is the required version."
        )
        return

    if current_version:
        printing.log_info(
            f"FLM v{current_version} is installed but is not the required version.  "
            f"Installing v{required_version}..."
        )
        verysilent = True
    else:
        printing.log_info(f"FLM not found. Installing FLM v{required_version}...")
        verysilent = False

    # Download the installer
    # pylint: disable=line-too-long
    installer_url = (
        "https://github.com/FastFlowLM/FastFlowLM/releases/download/v"
        + required_version
        + "/flm-setup.exe"
    )
    installer_path = os.path.join(tempfile.gettempdir(), "flm-setup.exe")
    installer_args = [installer_path, "/VERYSILENT"] if verysilent else [installer_path]

    try:
        # Remove existing installer if present
        if os.path.exists(installer_path):
            os.remove(installer_path)

        printing.log_info("Downloading FLM installer...")
        response = requests.get(installer_url, stream=True, timeout=30)
        response.raise_for_status()

        # Save installer to disk
        with open(installer_path, "wb") as f:
            for chunk in response.iter_content(chunk_size=8192):
                f.write(chunk)
            f.flush()
            os.fsync(f.fileno())

        printing.log_info(f"Downloaded FLM installer to {installer_path}")

        # Launch the installer GUI
        printing.log_warning(
            "Launching FLM installer GUI. Please complete the installation..."
            if not verysilent
            else "Installing FLM..."
        )

        # Launch installer and wait for it to complete
        if os.name == "nt":  # Windows
            process = subprocess.Popen(installer_args, shell=True)
        else:
            process = subprocess.Popen(installer_args)

        # Wait for installer to complete
        process.wait()

        if process.returncode != 0:
            raise RuntimeError(
                f"FLM installer failed with exit code {process.returncode}"
            )

        printing.log_info("FLM installer completed successfully")

        # Refresh environment to pick up new PATH entries
        refresh_environment()

        # Wait a moment for system to update
        time.sleep(2)

        # Verify installation
        max_retries = 10
        for attempt in range(max_retries):
            new_version, required_version = check_flm_version()
            if new_version and Version(new_version) == Version(required_version):
                printing.log_info(
                    f"FLM v{new_version} successfully installed and verified"
                )
                return

            if attempt < max_retries - 1:
                printing.log_warning(
                    f"FLM not yet available in PATH, retrying... (attempt {attempt+1}/{max_retries})",
                )
                time.sleep(3)
                refresh_environment()

        # Final check failed
        raise RuntimeError(
            "FLM installation completed but 'flm' command is not available in PATH. "
            "Please ensure FLM is properly installed and available in your system PATH."
        )

    except requests.RequestException as e:
        raise RuntimeError(f"Failed to download FLM installer: {e}") from e
    except Exception as e:
        raise RuntimeError(f"FLM installation failed: {e}") from e
    finally:
        # Clean up installer file
        if os.path.exists(installer_path):
            try:
                os.remove(installer_path)
            except OSError:
                pass  # Ignore cleanup errors


def download_flm_model(
    config_checkpoint, _=None, do_not_upgrade=False, capture_output=False
) -> dict:
    """
    Downloads the FLM model for the given configuration.

    Args:
        config_checkpoint: name of the FLM model to install.
        _: placeholder for `config_mmproj`, which is standard
            for WrappedServer (see llamacpp/utils.py) .
        do_not_upgrade: whether to re-download the model if it is already
            available.
    """

    if do_not_upgrade:
        command = ["flm", "pull", f"{config_checkpoint}"]
    else:
        command = ["flm", "pull", f"{config_checkpoint}", "--force"]

    subprocess.run(command, check=True, capture_output=capture_output)


def get_flm_installed_models() -> List[str]:
    """
    Parse FLM model list and return installed model checkpoints.

    Uses the improved FLM CLI methodology with --filter and --quiet flags
    for cleaner, more reliable output parsing.

    Returns:
        List of installed FLM model checkpoints (e.g., ["llama3.2:1b", "gemma3:4b"])
    """
    try:
        result = subprocess.run(
            ["flm", "list", "--filter", "installed", "--quiet"],
            capture_output=True,
            text=True,
            check=True,
            encoding="utf-8",
            errors="replace",
        )

        # Check if we got valid output
        if not result.stdout:
            return []

        installed_checkpoints = []

        lines = result.stdout.strip().split("\n")
        for line in lines:
            line = line.strip()
            # Skip the "Models:" header line
            if line == "Models:" or not line:
                continue
            # Parse model checkpoint (format: "  - modelname:tag")
            if line.startswith("- "):
                checkpoint = line[2:].strip()
                installed_checkpoints.append(checkpoint)

        return installed_checkpoints

    except (
        subprocess.CalledProcessError,
        FileNotFoundError,
        AttributeError,
        NotADirectoryError,
    ):
        # FLM not installed, not available, or output parsing failed
        return []


def is_flm_available() -> bool:
    """
    Check if FLM is available and meets minimum version requirements.
    """
    current_version, latest_version = check_flm_version()
    return current_version is not None and Version(current_version) == Version(
        latest_version
    )


def discard_output(process: subprocess.Popen):
    """
    Continuously read and discard output from the given process.
    """
    s = process.stdout
    if s is None:
        return
    for _ in iter(s.readline, ""):
        pass


def log_output(process: subprocess.Popen):
    """
    Continuously read and log output from the given process.
    """
    s = process.stdout
    if s is None:
        return

    for line in iter(s.readline, ""):
        line = line.strip()
        if line:
            printing.log_info(f"FLM server: {line}")


class FLMTokenizerAdapter(PassthroughTokenizer):
    pass


class FLMAdapter(ModelAdapter):
    def __init__(
        self,
        model,
        output_tokens,
        state=None,
    ):
        super().__init__()

        self.model = model
        self.output_tokens = (
            output_tokens  # default value of max tokens to generate from a prompt
        )
        self.state = state
        self.server_process = None
        self.server_port = None
        self.server_output_process = None

    def __del__(self):
        self.stop_server()

    def download(self, force=False):
        """
        Download the FLM model (if not already downloaded or if force flag is set).
        """
        try:
            download_flm_model(
                self.model, None, do_not_upgrade=(not force), capture_output=True
            )
        except Exception as e:
            error_msg = f"Failed to download FLM model: {str(e)}\n"
            error_msg += "Run 'flm list' to see list of valid FLM models."
            raise Exception(error_msg)

    def generate(
        self,
        input_ids: str,
        max_new_tokens: Optional[int] = None,
        temperature: float = 0.8,
        top_p: float = 0.95,
        save_max_memory_used: bool = False,
        **kwargs,  # pylint: disable=unused-argument
    ):
        """
        Pass a text prompt into the FLM inference CLI.

        The input_ids arg here should receive the original text that
        would normally be encoded by a tokenizer.

        Args:
            input_ids: The input text prompt
            temperature: Temperature for sampling (0.0 = greedy)
            top_p: Top-p sampling threshold
            **kwargs: Additional arguments (ignored)

        Returns:
            List containing a single string with the generated text, or raw output if
            return_raw=True
        """

        # Check server process is running
        if self.server_process is None:
            error_msg = "FLM server must be started before calling the generate method."
            raise Exception(error_msg)
        if self.server_process.poll() is not None:
            error_msg = (
                f"FLM server has exited with exit code: {self.server_process.poll()}"
            )
            raise Exception(error_msg)

        # Start memory monitoring in a separate thread
        if save_max_memory_used:
            memory_data = {}
            stop_event = threading.Event()
            monitor_thread = threading.Thread(
                target=monitor_process_memory,
                args=(self.server_process.pid, memory_data),
                kwargs={"stop_event": stop_event},
                daemon=True,
            )
            monitor_thread.start()

        if max_new_tokens is None:
            max_new_tokens = self.output_tokens
        prompt = input_ids
        response = self.send_request_to_server(prompt, max_new_tokens)
        response_content = response.choices[0].message.content
        finish_reason = response.choices[0].finish_reason
        usage = response.usage

        printing.log_info(f"FLM response: {response_content}")
        printing.log_info(f"FLM finish reason: {finish_reason}")

        # Extract info from usage
        #
        # Example:
        #   CompletionUsage(completion_tokens=27, prompt_tokens=39,
        #       total_tokens=66, completion_tokens_details=None,
        #       prompt_tokens_details=None, load_duration=1e-06,
        #       prefill_duration_ttft=1.026799104,
        #       decoding_duration=2.2763712, prefill_speed_tps=37.98211339304012,
        #       decoding_speed_tps=11.860982953922454)
        #
        self.response_tokens = getattr(usage, "completion_tokens", None)
        self.prompt_tokens = getattr(usage, "prompt_tokens", None)
        self.time_to_first_token = getattr(usage, "prefill_duration_ttft", None)
        self.tokens_per_second = getattr(usage, "decoding_speed_tps", None)

        # Signal monitor thread to stop and wait for it to finish
        if save_max_memory_used:
            stop_event.set()  # Signal the monitor to stop
            self.peak_wset = memory_data.get("peak_wset", None)

        return response_content

    def start_server(self, ctx_len=None, timeout=300):
        """
        Start the FLM server and save the process and port.
        """
        port = find_free_port()
        cmd = ["flm", "serve", self.model, "--port", str(port)]
        if ctx_len:
            cmd.extend(["--ctx-len", str(ctx_len)])
        self.server_process = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            encoding="utf-8",
            errors="replace",
        )

        # Wait for server to start
        server_ready = False
        start_time = time.time()
        while time.time() - start_time < timeout:
            line = self.server_process.stdout.readline()
            if not line:
                break
            line = line.strip()
            printing.log_info(f"FLM server: {line}")
            if "WebServer started on port" in line:
                try:
                    self.server_port = int(line.split("port")[1].split()[0])
                    server_ready = True
                    break
                except Exception as e:
                    self.server_process.terminate()
                    self.server_process = None
                    raise Exception(
                        f"Error occurred: {e}\nFailed to parse FLM server port from line: {line}"
                    )

        if not server_ready or self.server_port is None:
            self.stop_server()
            raise Exception(
                f"Server failed to start within {timeout} seconds.  Command was '{' '.join(cmd)}'."
            )

        # Start a thread to continuously read and log FLM server output
        self.server_output_process = threading.Thread(
            target=log_output,
            args=(self.server_process,),
            daemon=True,
        )
        self.server_output_process.start()

    def stop_server(self):
        if self.server_process:
            printing.log_info("Stopping FLM server...")
            self.server_process.stdin.write("exit\n")
            self.server_process.stdin.flush()
            self.server_process.terminate()
            self.server_process = None
        self.server_port = None
        if self.server_output_process:
            self.server_output_process.join()
            self.server_output_process = None

    def send_request_to_server(self, prompt, max_new_tokens):
        """
        Send prompt to the FLM server using the OpenAI client and return text result.
        """
        client = None
        try:

            # Connect to local FastFlowLM server using detected port
            client = OpenAI(
                base_url=f"http://localhost:{self.server_port}/v1",  # FastFlowLM endpoint
                api_key="flm",  # Dummy key (FastFlowLM doesn't require authentication)
            )

            # Use the passed input_str in the messages payload
            response = client.chat.completions.create(
                model=self.model,
                messages=[{"role": "user", "content": prompt}],
                temperature=0.9,
                top_p=0.95,
                presence_penalty=0.5,
                max_tokens=max_new_tokens,
            )
            return response

        except Exception as e:
            raise Exception(f"Error during processing prompt with FLM server: {e}")
