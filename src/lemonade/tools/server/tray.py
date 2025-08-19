import os
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
import subprocess
import webbrowser
from pathlib import Path
import logging
import tempfile
import requests
from packaging.version import parse as parse_version

from lemonade.version import __version__
from lemonade.tools.server.utils.system_tray import SystemTray, Menu, MenuItem


class OutputDuplicator:
    """
    Output duplicator that writes to both a file and a stream.
    """

    def __init__(self, file_path, stream):
        self.file = open(file_path, "a", encoding="utf-8")
        self.stream = stream

    def write(self, data):
        self.file.write(data)
        self.stream.write(data)
        self.file.flush()
        self.stream.flush()

    def flush(self):
        self.file.flush()
        self.stream.flush()

    def isatty(self):
        # Delegate the isatty check to the original stream
        return self.stream.isatty()


class LemonadeTray(SystemTray):
    """
    Lemonade-specific system tray implementation.
    """

    def __init__(self, log_file, port, server_factory, log_level="info"):
        # Find the icon path
        icon_path = Path(__file__).resolve().parents[0] / "static" / "favicon.ico"

        # Initialize the base class
        super().__init__("Lemonade Server", str(icon_path))

        # Lemonade-specific attributes
        self.loaded_llm = None
        self.server = None
        self.server_thread = None
        self.executor = ThreadPoolExecutor(max_workers=1)
        self.log_file = log_file
        self.port = port
        self.server_factory = server_factory
        self.debug_logs_enabled = log_level == "debug"

        # Get current and latest version
        self.current_version = __version__
        self.latest_version = __version__
        self.latest_version_url = None

        # We will get the models list with update_downloaded_models_background()
        # so that getting the list isn't on the critical path of startup
        self.downloaded_models = {}

        # Set up logger
        self.logger = logging.getLogger(__name__)

        # Initialize console handler (will be set up in run method)
        self.console_handler = None

        # Background thread for updating model mapping
        self.model_update_thread = None
        self.stop_model_update = threading.Event()

        # Background thread for version checking
        self.version_check_thread = None
        self.stop_version_check = threading.Event()

    def get_latest_version(self):
        """
        Update the latest version information.
        """
        try:
            # Prepare headers for GitHub API request
            headers = {}
            github_token = os.environ.get("GITHUB_TOKEN")
            if github_token:
                headers["Authorization"] = f"token {github_token}"

            response = requests.get(
                "https://api.github.com/repos/lemonade-sdk/lemonade/releases/latest",
                headers=headers,
                timeout=10,  # Add timeout to prevent hanging
            )
            response.raise_for_status()
            release_data = response.json()

            # Get version from tag name (typically in format "vX.Y.Z")
            self.latest_version = release_data.get("tag_name", "").lstrip("v")

            # Find the installer asset
            self.latest_version_url = None
            for asset in release_data.get("assets", []):
                if asset.get("name", "").endswith(
                    ".exe"
                ) and "Lemonade_Server_Installer" in asset.get("name", ""):
                    self.latest_version_url = asset.get("browser_download_url")
                    break

            self.logger.debug(
                f"Updated version info: latest version {self.latest_version}"
            )
        except Exception as e:  # pylint: disable=broad-exception-caught
            self.logger.error(f"Error fetching latest version: {str(e)}")
        return self.latest_version, self.latest_version_url

    def update_version_background(self):
        """
        Background thread function to update version information periodically.
        """
        self.get_latest_version()
        while not self.stop_version_check.wait(900):  # 900 seconds = 15 minutes
            try:
                self.get_latest_version()
            except Exception as e:  # pylint: disable=broad-exception-caught
                self.logger.error(f"Error updating version in background: {str(e)}")

    def update_downloaded_models_background(self):
        """
        Background thread function to update model mapping every 1 second until a valid
        response is received, then every 10 seconds after that.
        This is used to avoid a ~0.5s delay when opening the tray menu.
        """
        poll_interval = 1
        while not self.stop_model_update.wait(poll_interval):
            try:
                response = requests.get(
                    f"http://localhost:{self.port}/api/v0/models",
                    timeout=0.1,  # Add timeout
                )
                response.raise_for_status()

                # Update the model mapping
                models_endpoint_response = response.json().get("data")

                # Convert to dict
                self.downloaded_models = {
                    model["id"]: {"checkpoint": model["checkpoint"]}
                    for model in models_endpoint_response
                }
                self.logger.debug("Model mapping updated in background")

                # Increase the poling interval after we get our first response
                poll_interval = 10
            except Exception:  # pylint: disable=broad-exception-caught
                # Poll again later
                pass

    def unload_llms(self, _, __):
        """
        Unload the currently loaded LLM.
        """
        requests.post(f"http://localhost:{self.port}/api/v0/unload")

    def load_llm(self, _, __, model_name):
        """Load an LLM model."""

        # Create config for loading
        config = {"model_name": model_name}

        # Use the executor to make the request asynchronously
        self.executor.submit(
            lambda: requests.post(
                f"http://localhost:{self.port}/api/v0/load", json=config
            )
        )

    def show_logs(self, _, __):
        """
        Show the log file in a new window.
        """
        try:
            subprocess.Popen(
                [
                    "powershell",
                    "Start-Process",
                    "powershell",
                    "-ArgumentList",
                    f'"-NoExit", "Get-Content -Wait {self.log_file}"',
                ]
            )
        except Exception as e:  # pylint: disable=broad-exception-caught
            self.logger.error(f"Error opening logs: {str(e)}")

    def open_documentation(self, _, __):
        """
        Open the documentation in the default web browser.
        """
        webbrowser.open("https://lemonade-server.ai/docs/")

    def open_llm_chat(self, _, __):
        """
        Open the LLM chat in the default web browser.
        """
        webbrowser.open(f"http://localhost:{self.port}/#llm-chat")

    def open_model_manager(self, _, __):
        """
        Open the model manager in the default web browser.
        """
        webbrowser.open(f"http://localhost:{self.port}/#model-management")

    def check_server_state(self):
        """
        Check the server state using the health endpoint
        """
        try:
            response = requests.get(
                f"http://localhost:{self.port}/api/v0/health",
                timeout=0.1,  # Add timeout
            )
            response.raise_for_status()
            response_data = response.json()
            checkpoint = response_data.get("model_loaded")

            # Convert checkpoint to model name if possible
            if checkpoint:
                # Create a mapping from checkpoint to model name
                checkpoint_to_model = {
                    model_info["checkpoint"]: model_name
                    for model_name, model_info in self.downloaded_models.items()
                }
                # Use the model name if available, otherwise use the checkpoint
                self.loaded_llm = checkpoint_to_model.get(checkpoint, checkpoint)
            else:
                self.loaded_llm = None

            return True  # Successfully checked server state

        except Exception as e:  # pylint: disable=broad-exception-caught
            # Server might not be ready yet, so just log at debug level
            self.logger.debug(f"Error checking server state: {str(e)}")
            return False  # Failed to check server state

    def change_port(self, _, __, new_port):
        """
        Change the server port and restart the server.
        """
        try:
            # Stop the current server
            if self.server_thread and self.server_thread.is_alive():
                # Set should_exit flag on the uvicorn server instance
                if (
                    hasattr(self.server, "uvicorn_server")
                    and self.server.uvicorn_server
                ):
                    self.server.uvicorn_server.should_exit = True
                self.server_thread.join(timeout=2)

            # Update the port in both the tray and the server instance
            self.port = new_port
            if self.server:
                self.server.port = new_port

            # Restart the server
            self.server_thread = threading.Thread(target=self.start_server, daemon=True)
            self.server_thread.start()

            # Show notification
            self.show_balloon_notification(
                "Port Changed", f"Lemonade Server is now running on port {self.port}"
            )

        except Exception as e:  # pylint: disable=broad-exception-caught
            self.logger.error(f"Error changing port: {str(e)}")
            self.show_balloon_notification("Error", f"Failed to change port: {str(e)}")

    def _using_installer(self):
        """
        Check if the user is using the NSIS installer by checking for embeddable python
        """
        py_home = Path(sys.executable).parent
        pth_file = (
            py_home / f"python{sys.version_info.major}{sys.version_info.minor}._pth"
        )
        return pth_file.exists()

    def upgrade_to_latest(self, _, __):
        """
        Download and launch the Lemonade Server installer if the user is using the NSIS installer
        Otherwise, simply open the browser to the release page
        """

        # If the user installed from source, simple open their browser to the release page
        if not self._using_installer():
            webbrowser.open("https://github.com/lemonade-sdk/lemonade/releases/latest")
            return

        # Show notification that download is starting
        self.show_balloon_notification(
            "Upgrading Lemonade",
            "Downloading Lemonade Server Installer. Please wait...",
        )

        # Create temporary file for the installer
        installer_path = os.path.join(
            tempfile.gettempdir(), "Lemonade_Server_Installer.exe"
        )
        if os.path.exists(installer_path):
            os.remove(installer_path)

        # Download the installer
        response = requests.get(self.latest_version_url, stream=True)
        response.raise_for_status()

        # Save the installer to disk and force write to disk
        with open(installer_path, "wb") as f:
            for chunk in response.iter_content(chunk_size=8192):
                f.write(chunk)
            f.flush()
            os.fsync(f.fileno())

        # Launch the installer as a completely detached process
        # subprocess.DETACHED_PROCESS - Creates a process that's not attached to the console
        # subprocess.CREATE_NEW_PROCESS_GROUP - Creates a new process group
        # close_fds=True - Closes file descriptors to prevent inheritance
        subprocess.Popen(
            [installer_path],
            creationflags=subprocess.DETACHED_PROCESS
            | subprocess.CREATE_NEW_PROCESS_GROUP,
            close_fds=True,
            shell=True,
            cwd=tempfile.gettempdir(),
        )

        # No need to quit the application, the installer will handle it

    def toggle_debug_logs(self, _, __):
        """
        Toggle debug logs on and off.
        """
        try:
            new_level = "debug" if not self.debug_logs_enabled else "info"
            response = requests.post(
                f"http://localhost:{self.port}/api/v1/log-level",
                json={"level": new_level},
            )
            response.raise_for_status()
            self.debug_logs_enabled = not self.debug_logs_enabled
            self.show_balloon_notification(
                "Debug Logs",
                f"Debug logs {'enabled' if self.debug_logs_enabled else 'disabled'}",
            )
        except (FileNotFoundError, ValueError) as e:
            self.logger.error(f"Error toggling debug logs: {str(e)}")
            self.show_balloon_notification("Error", "Failed to toggle debug logs.")

    def create_menu(self):
        """
        Create the Lemonade-specific context menu.
        """
        # Check server health when menu is opened
        status_successfully_checked = self.check_server_state()

        items = []

        if not status_successfully_checked:
            items.append(
                MenuItem("Server Busy - See Logs for details", None, enabled=False)
            )
        elif self.loaded_llm:
            items.extend(
                [
                    MenuItem(f"Loaded: {self.loaded_llm}", None, enabled=False),
                    MenuItem("Unload LLM", self.unload_llms),
                ]
            )
        else:
            items.extend(
                [
                    MenuItem("No models loaded", None, enabled=False),
                ]
            )

        # Create menu items for all downloaded models
        model_menu_items = []
        if not self.downloaded_models:
            model_menu_items.append(
                MenuItem(
                    "No models available: Use the Model Manager to pull models",
                    None,
                    enabled=False,
                )
            )
        else:
            for model_name, _ in self.downloaded_models.items():
                # Create a function that returns the lambda to properly capture the variables
                def create_handler(mod):
                    return lambda icon, item: self.load_llm(icon, item, mod)

                model_item = MenuItem(model_name, create_handler(model_name))

                # Set checked property instead of modifying the text
                model_item.checked = model_name == self.loaded_llm
                model_menu_items.append(model_item)

        load_submenu = Menu(*model_menu_items)

        # Create port selection submenu with 3 options
        port_menu_items = []
        port_options = [
            8000,
            8020,
            8040,
            8060,
            8080,
            9000,
        ]

        for port_option in port_options:
            # Create a function that returns the lambda to properly capture the port variable
            def create_port_handler(port):
                return lambda icon, item: self.change_port(icon, item, port)

            # Set checked property instead of modifying the text
            port_item = MenuItem(
                f"Port {port_option}", create_port_handler(port_option)
            )
            port_item.checked = port_option == self.port
            port_menu_items.append(port_item)

        port_submenu = Menu(*port_menu_items)

        # Create the Logs submenu
        debug_log_text = "Enable Debug Logs"
        debug_log_item = MenuItem(debug_log_text, self.toggle_debug_logs)
        debug_log_item.checked = self.debug_logs_enabled

        logs_submenu = Menu(
            MenuItem("Show Logs", self.show_logs),
            Menu.SEPARATOR,
            debug_log_item,
        )

        if status_successfully_checked:
            items.append(MenuItem("Load Model", None, submenu=load_submenu))
        items.append(MenuItem("Port", None, submenu=port_submenu))
        items.append(Menu.SEPARATOR)

        # Only show upgrade option if newer version is available
        if parse_version(self.latest_version) > parse_version(self.current_version):
            items.append(
                MenuItem(
                    f"Upgrade to version {self.latest_version}", self.upgrade_to_latest
                )
            )

        items.append(MenuItem("Documentation", self.open_documentation))
        items.append(MenuItem("LLM Chat", self.open_llm_chat))
        items.append(MenuItem("Model Manager", self.open_model_manager))
        items.append(MenuItem("Logs", None, submenu=logs_submenu))
        items.append(Menu.SEPARATOR)
        items.append(MenuItem("Quit Lemonade", self.exit_app))
        return Menu(*items)

    def start_server(self):
        """
        Start the uvicorn server.
        """
        self.server = self.server_factory()
        self.server.uvicorn_server = self.server.run_in_thread(self.server.host)
        self.server.uvicorn_server.run()

    def run(self):
        """
        Run the Lemonade tray application.
        """

        # Register window class and create window
        self.register_window_class()
        self.create_window()

        # Set up Windows console control handler for CTRL+C
        self.console_handler = self.setup_console_control_handler(self.logger)

        # Add tray icon
        self.add_tray_icon()

        # Start the background model mapping update thread
        self.model_update_thread = threading.Thread(
            target=self.update_downloaded_models_background, daemon=True
        )
        self.model_update_thread.start()

        # Start the version check thread
        self.version_check_thread = threading.Thread(
            target=self.update_version_background, daemon=True
        )
        self.version_check_thread.start()

        # Start the server in a separate thread
        self.server_thread = threading.Thread(target=self.start_server, daemon=True)
        self.server_thread.start()

        # Show initial notification
        self.show_balloon_notification(
            "Woohoo!",
            (
                "Lemonade Server is running! "
                "Right-click the tray icon below to access options."
            ),
        )

        # Run the message loop in the main thread
        self.message_loop()

    def exit_app(self, icon, item):
        """
        Exit the application.
        """
        # Stop the background threads
        self.stop_model_update.set()
        self.stop_version_check.set()

        if self.model_update_thread and self.model_update_thread.is_alive():
            self.model_update_thread.join(timeout=1)
        if self.version_check_thread and self.version_check_thread.is_alive():
            self.version_check_thread.join(timeout=1)

        # Call parent exit method
        super().exit_app(icon, item)

        # Stop the server using the CLI stop command to ensure a rigorous cleanup
        # This must be a subprocess to ensure the cleanup doesnt kill itself
        subprocess.Popen(
            [sys.executable, "-m", "lemonade_server.cli", "stop"], shell=True
        )
