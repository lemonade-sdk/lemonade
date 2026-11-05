"""Drive an interactive CLI through a real POSIX terminal."""

import errno
import fcntl
import os
import pty
import select
import struct
import subprocess
import termios
import time


class TerminalSession:
    """Own a subprocess and its terminal, including deterministic cleanup."""

    def __init__(self, command, env, columns=120, rows=32):
        self.master, slave = pty.openpty()
        self.output = bytearray()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        try:
            self.process = subprocess.Popen(
                command, stdin=slave, stdout=slave, stderr=slave, env=env
            )
        except BaseException:
            os.close(self.master)
            raise
        finally:
            os.close(slave)

    def send(self, data):
        """Send literal input bytes, including terminal key sequences."""
        os.write(self.master, data)

    def _read(self, timeout):
        if not select.select([self.master], [], [], timeout)[0]:
            return False
        try:
            data = os.read(self.master, 65536)
        except OSError as error:
            if error.errno != errno.EIO:
                raise
            return False
        self.output.extend(data)
        return bool(data)

    def wait_for(self, text, timeout=10):
        """Wait for actual rendered output, not a fixed startup delay."""
        needle = text.encode()
        deadline = time.monotonic() + timeout
        while needle not in self.output:
            if time.monotonic() >= deadline or self.process.poll() is not None:
                while self._read(0):
                    pass
                if needle in self.output:
                    return
                raise AssertionError(
                    f"Terminal did not render {text!r}:\n"
                    + self.output.decode(errors="replace")[-8000:]
                )
            self._read(min(0.1, max(0, deadline - time.monotonic())))

    def wait(self, timeout=10):
        """Drain terminal output while waiting for the subprocess to exit."""
        deadline = time.monotonic() + timeout
        while self.process.poll() is None:
            if time.monotonic() >= deadline:
                raise AssertionError("Terminal subprocess did not exit")
            self._read(0.1)
        while self._read(0):
            pass
        return self.process.returncode

    def close(self):
        """Terminate a remaining subprocess and release its terminal."""
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait()
        os.close(self.master)
