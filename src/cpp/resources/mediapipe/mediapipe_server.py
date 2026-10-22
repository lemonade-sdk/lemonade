"""MediaPipe hand landmarks on the CPU, served to lemond over HTTP.

lemond launches this script with the Python of the venv it builds for the
`mediapipe` backend (or a user-supplied one) and speaks a small HTTP contract:

    GET  /health  -> 200 {"status": "ok", ...} once the model is ready
    POST /hands   {"images": ["<base64>", ...], "confidence": 0.5,
                   "max_hands": 2, "mirrored": false}
                  -> 200 {"data": [{"index", "width", "height", "hands"}],
                          "inference_ms": 12.4}

Each hand carries its handedness, a score, a bounding box and 21 landmarks in
pixels of the original image (`z` is MediaPipe's relative depth, in the same
scale as `x`, smaller is closer to the camera).
"""

from __future__ import annotations

import argparse
import base64
import binascii
import json
import sys
import threading
import time
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import numpy as np

LANDMARKS = [
    "wrist",
    "thumb_cmc", "thumb_mcp", "thumb_ip", "thumb_tip",
    "index_finger_mcp", "index_finger_pip", "index_finger_dip", "index_finger_tip",
    "middle_finger_mcp", "middle_finger_pip", "middle_finger_dip", "middle_finger_tip",
    "ring_finger_mcp", "ring_finger_pip", "ring_finger_dip", "ring_finger_tip",
    "pinky_mcp", "pinky_pip", "pinky_dip", "pinky_tip",
]  # fmt: skip

# Building a landmarker takes ~100 ms, and its thresholds are fixed at creation,
# so one is kept per (max_hands, confidence) a client actually uses.
MAX_LANDMARKERS = 4


def log(msg: str) -> None:
    print(f"mediapipe-server: {msg}", file=sys.stderr, flush=True)


class BadRequest(ValueError):
    pass


def find_task_file(model_path: Path) -> Path:
    if model_path.is_file():
        return model_path
    candidates = sorted(model_path.rglob("*.task"))
    if len(candidates) == 1:
        return candidates[0]
    raise FileNotFoundError(
        f"expected one .task file under {model_path}, found {len(candidates)}"
    )


class HandTracker:
    def __init__(self, args: argparse.Namespace):
        import cv2
        import mediapipe as mp
        from mediapipe.tasks.python import BaseOptions, vision

        self.cv2 = cv2
        self.mp = mp
        self.vision = vision
        self.base_options = BaseOptions
        self.task_path = find_task_file(Path(args.model_path))
        self.version = getattr(mp, "__version__", "unknown")
        self.lock = threading.Lock()
        self.landmarkers: OrderedDict[tuple[int, float], object] = OrderedDict()
        t0 = time.perf_counter()
        self._landmarker(2, 0.5)
        log(
            f"ready: {self.task_path} mediapipe={self.version} "
            f"load={1000 * (time.perf_counter() - t0):.0f}ms"
        )

    def _landmarker(self, max_hands: int, confidence: float):
        key = (max_hands, round(confidence, 3))
        if key in self.landmarkers:
            self.landmarkers.move_to_end(key)
            return self.landmarkers[key]
        options = self.vision.HandLandmarkerOptions(
            base_options=self.base_options(model_asset_path=str(self.task_path)),
            running_mode=self.vision.RunningMode.IMAGE,
            num_hands=max_hands,
            min_hand_detection_confidence=confidence,
            min_hand_presence_confidence=confidence,
            min_tracking_confidence=confidence,
        )
        landmarker = self.vision.HandLandmarker.create_from_options(options)
        self.landmarkers[key] = landmarker
        if len(self.landmarkers) > MAX_LANDMARKERS:
            _, oldest = self.landmarkers.popitem(last=False)
            oldest.close()
        return landmarker

    def decode(self, b64: str) -> np.ndarray:
        if b64.startswith("data:"):
            b64 = b64.split(",", 1)[-1]
        raw = base64.b64decode(b64, validate=False)
        img = self.cv2.imdecode(
            np.frombuffer(raw, dtype=np.uint8), self.cv2.IMREAD_COLOR
        )
        if img is None:
            raise ValueError("not a decodable image")
        return img

    def hands(
        self, frame_bgr: np.ndarray, confidence: float, max_hands: int, mirrored: bool
    ):
        h, w = frame_bgr.shape[:2]
        rgb = np.ascontiguousarray(frame_bgr[:, :, ::-1])
        image = self.mp.Image(image_format=self.mp.ImageFormat.SRGB, data=rgb)
        with self.lock:
            result = self._landmarker(max_hands, confidence).detect(image)
        hands = []
        for landmarks, handedness in zip(result.hand_landmarks, result.handedness):
            category = handedness[0]
            side = category.category_name.lower()
            # MediaPipe names handedness as seen in a mirrored (selfie) image; a
            # plain camera frame shows the person's other hand.
            if not mirrored and side in ("left", "right"):
                side = "right" if side == "left" else "left"
            points = [
                {
                    "name": LANDMARKS[i] if i < len(LANDMARKS) else f"landmark_{i}",
                    "x": round(lm.x * w, 2),
                    "y": round(lm.y * h, 2),
                    "z": round(lm.z * w, 2),
                }
                for i, lm in enumerate(landmarks)
            ]
            xs = [p["x"] for p in points]
            ys = [p["y"] for p in points]
            hands.append(
                {
                    "handedness": side,
                    "score": round(float(category.score), 5),
                    "box": {
                        "x1": round(max(min(xs), 0.0), 2),
                        "y1": round(max(min(ys), 0.0), 2),
                        "x2": round(min(max(xs), float(w)), 2),
                        "y2": round(min(max(ys), float(h)), 2),
                    },
                    "landmarks": points,
                }
            )
        hands.sort(key=lambda hand: hand["score"], reverse=True)
        return {"width": int(w), "height": int(h), "hands": hands}


def handle_hands(tracker: HandTracker, body: dict) -> dict:
    images = body.get("images")
    if isinstance(images, str):
        images = [images]
    if (
        not isinstance(images, list)
        or not images
        or not all(isinstance(i, str) for i in images)
    ):
        raise BadRequest("'images' must be a non-empty list of base64 strings")
    confidence = float(body.get("confidence", 0.5))
    max_hands = int(body.get("max_hands", 2))
    mirrored = bool(body.get("mirrored", False))
    if not 0.0 <= confidence <= 1.0 or not 1 <= max_hands <= 16:
        raise BadRequest("'confidence' must be in [0, 1] and 'max_hands' in [1, 16]")
    t0 = time.perf_counter()
    data = []
    for index, encoded in enumerate(images):
        try:
            frame = tracker.decode(encoded)
        except (ValueError, binascii.Error, OSError) as e:
            raise BadRequest(f"images[{index}] is not a decodable image: {e}") from e
        result = tracker.hands(frame, confidence, max_hands, mirrored)
        result["index"] = index
        data.append(result)
    return {"data": data, "inference_ms": round(1000 * (time.perf_counter() - t0), 2)}


def make_handler(tracker: HandTracker):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass

        def _send(self, status: int, payload: dict) -> None:
            body = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path.rstrip("/") == "/health":
                self._send(
                    200,
                    {
                        "status": "ok",
                        "model": str(tracker.task_path),
                        "mediapipe": tracker.version,
                    },
                )
            else:
                self._send(
                    404, {"error": {"message": "not found", "type": "not_found"}}
                )

        def do_POST(self):
            if self.path.rstrip("/") != "/hands":
                self._send(
                    404, {"error": {"message": "not found", "type": "not_found"}}
                )
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(length) or b"{}")
                if not isinstance(body, dict):
                    raise BadRequest("request body must be a JSON object")
                self._send(200, handle_hands(tracker, body))
            except (BadRequest, ValueError, TypeError) as e:
                self._send(
                    400, {"error": {"message": str(e), "type": "invalid_request_error"}}
                )
            except Exception as e:  # noqa: BLE001
                log(f"hands failed: {e!r}")
                self._send(500, {"error": {"message": str(e), "type": "server_error"}})

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(
        description="MediaPipe hand landmark server", allow_abbrev=False
    )
    parser.add_argument(
        "--model-path", required=True, help=".task file or directory holding one"
    )
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument(
        "--status-file", default="", help="startup errors are written here"
    )
    args = parser.parse_args()

    try:
        tracker = HandTracker(args)
        server = ThreadingHTTPServer((args.host, args.port), make_handler(tracker))
    except Exception as e:  # noqa: BLE001
        log(f"startup failed: {e}")
        if args.status_file:
            Path(args.status_file).write_text(
                json.dumps({"error": str(e)}), encoding="utf-8"
            )
        return 1
    server.daemon_threads = True
    log(f"listening on http://{args.host}:{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
