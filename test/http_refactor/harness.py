"""Isolated, standard-library HTTP harness for lemond contract tests."""

from __future__ import annotations

from dataclasses import dataclass
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
_MISSING = object()


def find_lemond(requested: str | None) -> Path:
    if requested:
        path = Path(requested).expanduser().resolve()
        if path.is_file() and os.access(path, os.X_OK):
            return path
        raise FileNotFoundError(f"Executable lemond not found: {path}")
    for candidate in (
        ROOT / "build" / "lemond",
        ROOT / "build" / "bin" / "lemond",
        ROOT / "build" / "Release" / "lemond",
        ROOT / "build-debug" / "lemond",
    ):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    raise FileNotFoundError("Build this checkout's lemond and supply --lemond PATH")


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@dataclass(frozen=True)
class Response:
    status: int
    headers: dict[str, str]
    body: bytes

    @property
    def json(self) -> Any:
        return json.loads(self.body)

    @property
    def text(self) -> str:
        return self.body.decode("utf-8", errors="replace")

    def header(self, name: str) -> str:
        return self.headers.get(name.lower(), "")


class Client:
    def __init__(self, port: int, token: str = "") -> None:
        self.port = port
        self.token = token

    def request(
        self,
        method: str,
        path: str,
        *,
        data: object = _MISSING,
        raw: bytes | None = None,
        headers: dict[str, str] | None = None,
        timeout: float = 12,
    ) -> Response:
        if not path.startswith("/"):
            raise ValueError(f"Absolute URL path required, received {path!r}")
        if raw is not None and data is not _MISSING:
            raise ValueError("Provide data or raw, not both")
        outgoing = dict(headers or {})
        if self.token and not any(k.lower() == "authorization" for k in outgoing):
            outgoing["Authorization"] = f"Bearer {self.token}"
        body = raw
        if data is not _MISSING:
            body = json.dumps(data).encode("utf-8")
            outgoing.setdefault("Content-Type", "application/json")
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        try:
            conn.request(method, path, body=body, headers=outgoing)
            res = conn.getresponse()
            content = res.read()
            return Response(
                res.status,
                {key.lower(): value for key, value in res.getheaders()},
                content,
            )
        finally:
            conn.close()

    def get(self, path: str, **kwargs: Any) -> Response:
        return self.request("GET", path, **kwargs)

    def post(self, path: str, *, data: object = _MISSING, **kwargs: Any) -> Response:
        return self.request("POST", path, data=data, **kwargs)

    def delete(self, path: str, **kwargs: Any) -> Response:
        return self.request("DELETE", path, **kwargs)


def multipart(
    fields: dict[str, str], files: dict[str, tuple[str, bytes]] | None = None
):
    boundary = "lemonade-refactor-verified-boundary"
    parts = []
    for name, value in fields.items():
        parts.append(
            f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n'.encode(
                "ascii"
            )
            + value.encode("utf-8")
            + b"\r\n"
        )
    for name, (filename, content) in (files or {}).items():
        parts.append(
            f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"; '
            f'filename="{filename}"\r\nContent-Type: audio/wav\r\n\r\n'.encode("ascii")
            + content
            + b"\r\n"
        )
    parts.append(f"--{boundary}--\r\n".encode("ascii"))
    return b"".join(parts), {
        "Content-Type": f"multipart/form-data; boundary={boundary}"
    }


class IsolatedLemond:
    def __init__(
        self,
        binary: str | Path,
        *,
        api_key: str = "",
        admin_key: str = "",
        options: dict[str, Any] | None = None,
    ) -> None:
        self.binary = str(Path(binary).resolve())
        self.tmp = tempfile.TemporaryDirectory(prefix="lemonade-http-refactor-")
        self.root = Path(self.tmp.name)
        self.config = self.root / "config"
        self.models = self.root / "models"
        self.port = free_port()
        self.log_path = self.root / "lemond.log"
        self.api_key = api_key
        self.admin_key = admin_key
        self.options = options or {}
        self.proc: subprocess.Popen | None = None
        self._log_file = None
        self.api = Client(self.port, api_key or admin_key)
        self.admin = Client(self.port, admin_key)
        self.anonymous = Client(self.port)
        for directory in (self.config, self.models):
            directory.mkdir(parents=True, exist_ok=True)

    def start(self) -> "IsolatedLemond":
        settings = {
            "config_version": 2,
            "offline": True,
            "no_fetch_executables": True,
            "auto_check_model_updates": False,
            "models_dir": str(self.models),
            "model_catalogs": [],
            "broadcast": False,
        }
        settings.update(self.options)
        if Path(settings["models_dir"]).resolve() != self.models.resolve():
            raise ValueError("Tests must use the isolated models directory")
        (self.config / "config.json").write_text(json.dumps(settings), encoding="utf-8")
        env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith("LEMONADE_")
            and key
            not in (
                "HF_HOME",
                "HF_HUB_CACHE",
                "MODELSCOPE_CACHE",
                "XDG_CACHE_HOME",
                "XDG_CONFIG_HOME",
            )
        }
        env.update(
            {
                "HOME": str(self.root),
                "HF_HOME": str(self.root / "hf"),
                "HF_HUB_CACHE": str(self.models),
                "XDG_CACHE_HOME": str(self.root / "xdg-cache"),
                "XDG_CONFIG_HOME": str(self.root / "xdg-config"),
                "LEMONADE_DISABLE_TELEMETRY": "1",
            }
        )
        if self.api_key:
            env["LEMONADE_API_KEY"] = self.api_key
        if self.admin_key:
            env["LEMONADE_ADMIN_API_KEY"] = self.admin_key
        self._log_file = self.log_path.open("wb")
        self.proc = subprocess.Popen(
            [self.binary, str(self.config), "--port", str(self.port), "--no-broadcast"],
            stdin=subprocess.DEVNULL,
            stdout=self._log_file,
            stderr=subprocess.STDOUT,
            env=env,
        )
        deadline = time.monotonic() + 45
        try:
            while time.monotonic() < deadline:
                if self.proc.poll() is not None:
                    raise RuntimeError("lemond exited during startup")
                try:
                    health = self.api.get("/api/v1/health", timeout=2)
                    if health.status == 200:
                        admin = self.admin if self.admin_key else self.anonymous
                        config = admin.get("/internal/config", timeout=3)
                        if config.status != 200 or config.json.get("models_dir") != str(
                            self.models
                        ):
                            raise RuntimeError("lemond ignored isolated models_dir")
                        return self
                except (OSError, TimeoutError):
                    pass
                time.sleep(0.2)
            raise RuntimeError("lemond did not become healthy")
        except Exception as error:
            detail = self.log_tail()
            self.close()
            raise RuntimeError(f"{error}\n--- lemond log ---\n{detail}") from error

    def log_tail(self) -> str:
        if self._log_file:
            self._log_file.flush()
        if self.log_path.exists():
            return self.log_path.read_text(errors="replace")[-6000:]
        return "(no log)"

    def close(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        if self._log_file:
            self._log_file.close()
            self._log_file = None
        self.tmp.cleanup()

    def __enter__(self) -> "IsolatedLemond":
        return self.start()

    def __exit__(self, *_args: Any) -> None:
        self.close()
