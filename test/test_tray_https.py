import os
import shutil
import subprocess
import sys
import tempfile
import time
import http.server
import ssl
import threading


class MockHTTPSHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/api/v1/health":
            self.server.health_checked = True
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        pass  # Suppress output


def run_test():
    with tempfile.TemporaryDirectory() as tmpdir:
        key_path = os.path.join(tmpdir, "key.pem")
        cert_path = os.path.join(tmpdir, "cert.pem")

        # Generate self-signed certificate
        try:
            subprocess.run(
                [
                    "openssl",
                    "req",
                    "-new",
                    "-newkey",
                    "rsa:2048",
                    "-days",
                    "1",
                    "-nodes",
                    "-x509",
                    "-subj",
                    "/CN=localhost",
                    "-keyout",
                    key_path,
                    "-out",
                    cert_path,
                ],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception as e:
            print(f"Error generating self-signed cert: {e}", file=sys.stderr)
            sys.exit(1)

        # Start Mock HTTPS Server on ephemeral port
        server = http.server.HTTPServer(("127.0.0.1", 0), MockHTTPSHandler)
        server.health_checked = False
        port = server.server_port

        # Wrap socket with SSL
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certfile=cert_path, keyfile=key_path)
        server.socket = context.wrap_socket(server.socket, server_side=True)

        server_thread = threading.Thread(target=server.serve_forever)
        server_thread.daemon = True
        server_thread.start()

        # Accept either a build tree or an installed package.
        override = os.environ.get("LEMONADE_TRAY_BINARY")
        if override:
            binary_path = override
        else:
            binary_path = "./build/lemonade-tray"
            if not os.path.exists(binary_path):
                binary_path = shutil.which("lemonade-tray") or binary_path
        if not os.path.exists(binary_path):
            print(f"lemonade-tray binary not found at {binary_path}", file=sys.stderr)
            sys.exit(1)

        runtime_dir = os.path.join(tmpdir, "runtime")
        os.makedirs(runtime_dir, mode=0o700, exist_ok=True)

        # Launch lemonade-tray pointing to mock HTTPS server
        env = os.environ.copy()
        env["LEMONADE_SKIP_VERIFY"] = "1"
        env["XDG_RUNTIME_DIR"] = runtime_dir
        env["RUNTIME_DIRECTORY"] = runtime_dir

        proc = subprocess.Popen(
            [binary_path, "--host", f"https://localhost:{port}", "--port", str(port)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )

        # Poll until mock HTTPS server receives a health check request
        start_time = time.time()
        success = False
        while time.time() - start_time < 5:
            if getattr(server, "health_checked", False):
                success = True
                break

            if proc.poll() is not None:
                # Process exited early
                break

            time.sleep(0.1)

        proc.terminate()
        stdout_data = ""
        stderr_data = ""
        try:
            stdout_data, stderr_data = proc.communicate(timeout=1)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                stdout_data, stderr_data = proc.communicate(timeout=1)
            except Exception:
                pass

        server.shutdown()
        server.server_close()

        if success:
            print("[PASS] HTTPS Tray E2E Smoke Test passed successfully.")
            sys.exit(0)
        else:
            print("[FAIL] HTTPS Tray E2E Smoke Test failed.", file=sys.stderr)
            print(f"Stdout: {stdout_data}", file=sys.stderr)
            print(f"Stderr: {stderr_data}", file=sys.stderr)
            sys.exit(1)


if __name__ == "__main__":
    run_test()
