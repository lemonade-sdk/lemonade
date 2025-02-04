import os
import time
from multiprocessing import Process, Queue
import matplotlib.pyplot as plt
import psutil
import yaml


DEFAULT_TRACK_MEMORY_INTERVAL = 0.25
MEMORY_USAGE_YAML_FILENAME = "memory_usage.yaml"
MEMORY_USAGE_PNGL_FILENAME = "memory_usage.png"


class MemoryTracker:

    @staticmethod
    def get_time_mem_list(process):
        return [time.time(), process.memory_info().rss]

    def __init__(self):
        self.process_being_tracked = None
        self.build_dir = None
        self.queue = None
        self.tracker_process = None
        self.tracking_active = False
        self.yaml_path = None

    def start(
        self, track_pid, build_dir, track_memory_interval=DEFAULT_TRACK_MEMORY_INTERVAL
    ):
        if self.tracking_active:
            raise RuntimeError("Cannot start tracking while already tracking")

        # Get the process being tracked
        self.process_being_tracked = psutil.Process(track_pid)

        # Save the folder where data and plot will be stored
        self.build_dir = build_dir

        # Create queue for passing messages to the tracker
        self.queue = Queue()

        # The yaml file where the memory usage data will be saved
        self.yaml_path = os.path.join(build_dir, MEMORY_USAGE_YAML_FILENAME)

        # Create process to continuously sample memory usage
        self.tracker_process = Process(
            target=self._memory_tracker_,
            args=(
                track_pid,
                self.queue,
                self.yaml_path,
                track_memory_interval,
            ),
        )
        self.tracker_process.start()
        self.tracking_active = True
        self.set_label("start")
        self.sample()

    def set_label(self, label):
        if self.tracking_active:
            self.queue.put(label)

    def sample(self):
        if self.tracking_active:
            self.queue.put(MemoryTracker.get_time_mem_list(self.process_being_tracked))

    def stop(self):
        if self.tracking_active:
            self.queue.put(None)
            self.tracking_active = False

    def create_plot(self, build_name: None):
        if self.tracker_process is None:
            return None

        if self.tracking_active:
            self.stop()

        # Wait for memory tracker to finish writing yaml data file
        while self.tracker_process.is_alive():
            self.tracker_process.join(timeout=0.5)

        try:
            with open(self.yaml_path, "r", encoding="utf-8") as f:
                memory_tracks = yaml.safe_load(f)
        except FileNotFoundError as e:
            print(f"Memory tracker file not found: {e.filename}")
            return None

        # Find final time in the startup track (first track) to subtract from all other times
        _, track = memory_tracks[0]
        t0 = track[-1][0]

        # last_t and last_y are used to draw a line between the last point of the prior
        # track and the first point of the current track
        last_t = None
        last_y = None

        plt.figure()
        for k, v in memory_tracks[1:]:
            if len(v) > 0:
                t = [x[0] - t0 for x in v]
                y = [float(x[1]) / 1024**3 for x in v]
                # draw new memory usage track
                if last_t is not None:
                    plt.plot([last_t] + t, [last_y] + y, label=k, marker=".")
                else:
                    plt.plot(t, y, label=k, marker=".")
                last_t = t[-1]
                last_y = y[-1]
        plt.xlabel("Time (sec)")
        plt.ylabel("GB")
        title_str = "Physical Memory Usage"
        if build_name is not None:
            title_str += "\n" + build_name
        plt.title(title_str)
        plt.legend()
        plt.grid()
        plot_path = os.path.join(self.build_dir, MEMORY_USAGE_PNGL_FILENAME)
        plt.savefig(plot_path)

        return plot_path

    @staticmethod
    def _memory_tracker_(
        tracked_pid,
        input_queue: Queue,
        yaml_path: str,
        track_memory_interval: float,
    ):
        """
        Tracks memory usage during build and saves to yaml file
        The build communicates with the tracker though the input_queue.  It may pass:
          1) string - This is to indicate that a new track is starting and the string is the label
                    for the next segment.  The tracker will automatically track memory usage at
                    the track_memory_interval once a first track_name is given to it.
          2) list - A time and a current memory usage value that is added to the current track
                    (typically used at the end of a segment to make sure that each segment is
                    sampled at least once
          3) None - This indicates that the tracker should stop tracking, save its data to a file
                    and end
        """
        memory_tracks = []
        current_track = []
        track_name = None
        tracker_exit = False

        try:
            tracked_process = psutil.Process(tracked_pid)
            while (
                not tracker_exit and tracked_process.status() == psutil.STATUS_RUNNING
            ):

                time.sleep(track_memory_interval)

                # Read any messages from the parent process
                while not input_queue.empty():
                    try:
                        message = input_queue.get(timeout=0.001)
                        if message is None or isinstance(message, str):
                            # Save current track.
                            if track_name is not None:
                                memory_tracks.append([track_name, current_track])
                            track_name = message
                            current_track = []
                            if message is None:
                                # Wrap up
                                tracker_exit = True
                                break
                        elif isinstance(message, list):
                            # Add time and memory data to current track
                            if track_name is not None:
                                current_track.append(message)
                            else:
                                raise TypeError(
                                    "Track name must be passed to memory tracker prior to "
                                    "sending data"
                                )
                        else:
                            raise TypeError(
                                "Unrecognized message type in memory_tracker input queue: "
                                f"{message}"
                            )

                    except input_queue.Empty:
                        # input_queue.empty had not been updated
                        pass

                if not tracker_exit and track_name is not None:
                    # Save current time and memory usage
                    current_track.append(
                        MemoryTracker.get_time_mem_list(tracked_process)
                    )

            # Save the collected memory tracks
            with open(yaml_path, "w", encoding="utf-8") as f:
                yaml.dump(memory_tracks, f)

        except psutil.NoSuchProcess:
            # If the parent process stopped existing, we can
            # safely assume that tracking is no longer needed
            # NOTE: this only seems to be needed on Windows
            pass
