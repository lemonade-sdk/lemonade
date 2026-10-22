"""Object detection on the AMD Ryzen AI NPU, served to lemond over HTTP.

lemond launches this script with the Python interpreter of a Ryzen AI Software
installation (the environment whose onnxruntime ships the
VitisAIExecutionProvider) and speaks a small HTTP contract with it:

    GET  /health  -> 200 {"status": "ok", ...} once the model session is ready
    POST /detect  {"images": ["<base64>", ...], "confidence": 0.25,
                   "classes": ["ball", ...], "max_detections": 100}
                  -> 200 {"data": [{"index", "width", "height", "detections"}],
                          "unresolved_classes": [...], "inference_ms": 7.1}

Supported ONNX detector layouts (picked from the graph outputs):

    end2end  [1, N, 6]       xyxy, score, class (Ultralytics YOLO26 / YOLOv10)
    detr     logits [1, N, C] + pred_boxes [1, N, 4]: sigmoid scores, normalized
             cxcywh boxes (RT-DETR and YOLO26 Hugging Face exports; not the
             softmax + no-object head of the original DETR)
    anchors  [1, 4 + C, A]   cxcywh + class scores (YOLOv8-12), NMS applied here

Inputs are letterboxed to the model size with gray padding and scaled to [0, 1],
the Ultralytics preprocessing.
"""

from __future__ import annotations

import argparse
import ast
import base64
import binascii
import hashlib
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REEXEC_FLAG = "_LEMONADE_RYZENAI_VISION_ENV"


def _ryzen_ai_linux_env() -> dict[str, str] | None:
    """Loader environment the VitisAI EP needs on Linux, or None if already set.

    The EP's native libraries live inside the Ryzen AI venv but the venv's
    activate script is what normally puts them on LD_LIBRARY_PATH. lemond runs
    the interpreter directly, so recreate that environment here.
    """
    import sysconfig

    if os.environ.get(REEXEC_FLAG) == "1":
        return None
    site = Path(sysconfig.get_paths()["purelib"])
    lib_dirs = [
        site / "flexml" / "flexml_extras" / "lib",
        site / "flexml" / "lib",
        site / "flexmlrt" / "lib",
        site / "voe" / "lib",
        site / "onnxruntime" / "capi",
    ]
    env = dict(os.environ)
    xrt = Path(env.get("XILINX_XRT", "/opt/xilinx/xrt"))
    if (xrt / "lib").is_dir():
        env["XILINX_XRT"] = str(xrt)
        lib_dirs.append(xrt / "lib")
    current = [p for p in env.get("LD_LIBRARY_PATH", "").split(":") if p]
    wanted = [str(d) for d in lib_dirs if d.is_dir() and str(d) not in current]
    env["LD_LIBRARY_PATH"] = ":".join(wanted + current)
    env.setdefault("RYZEN_AI_INSTALLATION_PATH", sys.prefix)
    env[REEXEC_FLAG] = "1"
    return env


if sys.platform.startswith("linux"):
    _env = _ryzen_ai_linux_env()
    if _env is not None:
        # The dynamic loader reads LD_LIBRARY_PATH only at process start, so the
        # new value takes effect for the EP's DT_NEEDED libraries only after an
        # exec. exec keeps the PID lemond is supervising.
        os.execve(sys.executable, [sys.executable] + sys.argv, _env)
elif sys.platform == "win32":
    # A conda env's native DLLs are on PATH only when the env is activated;
    # lemond starts its python.exe directly.
    _conda_bin = Path(sys.prefix) / "Library" / "bin"
    if _conda_bin.is_dir():
        os.add_dll_directory(str(_conda_bin))
        os.environ["PATH"] = f"{_conda_bin};{os.environ.get('PATH', '')}"

import numpy as np  # noqa: E402

# Bounds decoder memory: a few KB of JPEG can otherwise declare a gigapixel image.
os.environ.setdefault("OPENCV_IO_MAX_IMAGE_PIXELS", str(8192 * 8192))
try:
    import cv2  # noqa: E402
except ImportError:
    cv2 = None

COCO80 = [
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
    "boat", "traffic light", "fire hydrant", "stop sign", "parking meter", "bench",
    "bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
    "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove",
    "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup",
    "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush",
]  # fmt: skip

# Everyday names for COCO classes, so a spoken request ("pick up the ball")
# grounds to a class without retraining.
SYNONYMS = {
    "ball": "sports ball",
    "phone": "cell phone",
    "mobile": "cell phone",
    "glass": "wine glass",
    "mug": "cup",
    "drink": "bottle",
    "water bottle": "bottle",
    "computer": "laptop",
    "plant": "potted plant",
    "teddy": "teddy bear",
    "bear": "teddy bear",
    "controller": "remote",
}

PAD_VALUE = 114
NMS_IOU = 0.7


def log(msg: str) -> None:
    print(f"ryzenai-vision-server: {msg}", file=sys.stderr, flush=True)


def resolve_class(name: str, labels: list[str]) -> str | None:
    """Model label for a class name or phrase ("the red ball"), or None."""
    by_key = {label.casefold(): label for label in labels}
    words = (name or "").casefold().split()
    if not words:
        return None
    # Longest phrase first so "water bottle" beats "bottle" and "teddy bear"
    # beats "bear"; whole words only so "carpet" never matches "car".
    for size in range(len(words), 0, -1):
        for start in range(len(words) - size + 1):
            phrase = " ".join(words[start : start + size])
            if phrase in by_key:
                return by_key[phrase]
            alias = SYNONYMS.get(phrase)
            if alias is not None and alias.casefold() in by_key:
                return by_key[alias.casefold()]
    return None


def find_onnx(model_path: Path) -> Path:
    if model_path.is_file():
        return model_path
    preferred = sorted(model_path.rglob("model.onnx"))
    if len(preferred) == 1:
        return preferred[0]
    candidates = sorted(model_path.rglob("*.onnx"))
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise FileNotFoundError(f"no .onnx file under {model_path}")
    raise FileNotFoundError(
        f"{len(candidates)} .onnx files under {model_path}; pass the file path: "
        + ", ".join(str(c) for c in candidates)
    )


def _indexed_labels(mapping) -> list[str] | None:
    """{class index: name} -> list where position == class index."""
    if not isinstance(mapping, dict) or not mapping:
        return None
    try:
        indexed = {int(k): str(v) for k, v in mapping.items()}
    except (TypeError, ValueError):
        return None
    if min(indexed) < 0:
        return None
    return [indexed.get(i, f"class_{i}") for i in range(max(indexed) + 1)]


def load_labels(onnx_path: Path, session_meta: dict[str, str]) -> list[str]:
    for config in (
        onnx_path.parent / "config.json",
        onnx_path.parent.parent / "config.json",
    ):
        if config.is_file():
            try:
                id2label = json.loads(config.read_text(encoding="utf-8")).get(
                    "id2label"
                )
            except (OSError, ValueError, AttributeError):
                id2label = None
            labels = _indexed_labels(id2label)
            if labels:
                return labels
    names = session_meta.get("names")
    if names:
        try:
            labels = _indexed_labels(ast.literal_eval(names))
        except (ValueError, SyntaxError):
            labels = None
        if labels:
            return labels
    return list(COCO80)


def decode_image(b64: str) -> np.ndarray:
    """Base64 (optionally a data: URL) -> BGR uint8 HxWx3."""
    if b64.startswith("data:"):
        b64 = b64.split(",", 1)[-1]
    raw = base64.b64decode(b64, validate=False)
    try:
        img = cv2.imdecode(np.frombuffer(raw, dtype=np.uint8), cv2.IMREAD_COLOR)
    except cv2.error as e:
        raise ValueError(str(e).strip().splitlines()[-1]) from e
    if img is None:
        raise ValueError("not a decodable image")
    return img


def letterbox(
    frame_bgr: np.ndarray, imgsz: int
) -> tuple[np.ndarray, float, float, float]:
    h, w = frame_bgr.shape[:2]
    ratio = min(imgsz / w, imgsz / h)
    new_w, new_h = int(round(w * ratio)), int(round(h * ratio))
    pad_x, pad_y = (imgsz - new_w) / 2, (imgsz - new_h) / 2
    resized = cv2.resize(frame_bgr, (new_w, new_h), interpolation=cv2.INTER_LINEAR)
    canvas = np.full((imgsz, imgsz, 3), PAD_VALUE, dtype=np.uint8)
    top, left = int(round(pad_y - 0.1)), int(round(pad_x - 0.1))
    canvas[top : top + new_h, left : left + new_w] = resized
    rgb = cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB)
    nchw = rgb.transpose(2, 0, 1)[None].astype(np.float32) / 255.0
    return np.ascontiguousarray(nchw), ratio, pad_x, pad_y


def nms(boxes: np.ndarray, scores: np.ndarray, iou: float) -> list[int]:
    order = scores.argsort()[::-1]
    keep: list[int] = []
    areas = (boxes[:, 2] - boxes[:, 0]).clip(0) * (boxes[:, 3] - boxes[:, 1]).clip(0)
    while order.size:
        i = int(order[0])
        keep.append(i)
        rest = order[1:]
        xx1 = np.maximum(boxes[i, 0], boxes[rest, 0])
        yy1 = np.maximum(boxes[i, 1], boxes[rest, 1])
        xx2 = np.minimum(boxes[i, 2], boxes[rest, 2])
        yy2 = np.minimum(boxes[i, 3], boxes[rest, 3])
        inter = (xx2 - xx1).clip(0) * (yy2 - yy1).clip(0)
        overlap = inter / np.maximum(areas[i] + areas[rest] - inter, 1e-9)
        order = rest[overlap <= iou]
    return keep


def cxcywh_to_xyxy(b: np.ndarray) -> np.ndarray:
    half_w, half_h = b[:, 2] / 2, b[:, 3] / 2
    return np.stack(
        [b[:, 0] - half_w, b[:, 1] - half_h, b[:, 0] + half_w, b[:, 1] + half_h], 1
    )


class Detector:
    def __init__(self, args: argparse.Namespace):
        import onnxruntime as ort

        if cv2 is None:
            raise RuntimeError(
                "opencv-python is not installed in this Ryzen AI environment "
                f"({sys.prefix}); pip install opencv-python-headless there"
            )
        self.ort = ort
        self.vaip_config = Path(args.vaip_config)
        self.onnx_path = find_onnx(Path(args.model_path))
        self.lock = threading.Lock()

        # A throwaway CPU session reads the I/O signature without paying for an
        # NPU compile, and fails fast on a corrupt or non-ONNX file.
        probe = ort.InferenceSession(
            str(self.onnx_path), providers=["CPUExecutionProvider"]
        )
        inp = probe.get_inputs()[0]
        self.input_name = inp.name
        spatial = [d for d in inp.shape[2:4] if isinstance(d, int) and d > 0]
        self.imgsz = spatial[0] if spatial else int(args.imgsz)
        self.layout = self._detect_layout(probe)
        self.labels = load_labels(
            self.onnx_path, probe.get_modelmeta().custom_metadata_map
        )
        if self.layout == "anchors":
            num_classes = probe.get_outputs()[0].shape[1] - 4
            if num_classes != len(self.labels):
                raise RuntimeError(
                    f"output has {num_classes} class scores but the model declares "
                    f"{len(self.labels)} labels; pose/segmentation heads are not "
                    "detectors"
                )
        symbolic = {
            pos: dim
            for pos, dim in enumerate(inp.shape)
            if not (isinstance(dim, int) and dim > 0)
        }
        del probe

        opts = ort.SessionOptions()
        opts.log_severity_level = 3
        fixed = {0: 1, 1: 3, 2: self.imgsz, 3: self.imgsz}
        for pos, dim in symbolic.items():
            if isinstance(dim, str) and dim and pos in fixed:
                opts.add_free_dimension_override_by_name(dim, fixed[pos])

        self.device = args.device
        if self.device == "npu":
            if "VitisAIExecutionProvider" not in ort.get_available_providers():
                raise RuntimeError(
                    f"onnxruntime at {Path(ort.__file__).parent} has no VitisAIExecutionProvider; "
                    "point ryzenaivision.npu_bin at the Python of a Ryzen AI Software install"
                )
            cache_dir = Path(args.cache_dir)
            cache_dir.mkdir(parents=True, exist_ok=True)
            cache_key = args.cache_key or self._default_cache_key()
            log(
                f"building NPU session for {self.onnx_path.name} (cache {cache_dir / cache_key}); "
                "the first compile of a model can take several minutes"
            )
            self.session = ort.InferenceSession(
                str(self.onnx_path),
                sess_options=opts,
                providers=["VitisAIExecutionProvider"],
                provider_options=[
                    {
                        "config_file": str(self.vaip_config),
                        "cache_dir": str(cache_dir),
                        "cache_key": cache_key,
                        "target": "VAIML",
                    }
                ],
            )
        else:
            self.session = ort.InferenceSession(
                str(self.onnx_path),
                sess_options=opts,
                providers=["CPUExecutionProvider"],
            )
        self.providers = self.session.get_providers()
        warm = np.zeros((1, 3, self.imgsz, self.imgsz), dtype=np.float32)
        t0 = time.perf_counter()
        self.session.run(None, {self.input_name: warm})
        log(
            f"ready: {self.onnx_path} layout={self.layout} imgsz={self.imgsz} "
            f"classes={len(self.labels)} providers={self.providers} "
            f"warmup={1000 * (time.perf_counter() - t0):.0f}ms"
        )

    def _default_cache_key(self) -> str:
        """Changes with the model, the compiler config, or the Ryzen AI release,
        so an upgrade never reuses a stale NPU compile."""
        from importlib import metadata

        digest = hashlib.sha256()
        with open(self.onnx_path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                digest.update(chunk)
        digest.update(self.vaip_config.read_bytes())
        for package in ("voe", "onnxruntime-vitisai"):
            try:
                digest.update(f"{package}={metadata.version(package)}".encode())
            except metadata.PackageNotFoundError:
                pass
        return f"{self.onnx_path.stem}_{digest.hexdigest()[:16]}"

    @staticmethod
    def _detect_layout(session) -> str:
        outs = session.get_outputs()
        names = {o.name for o in outs}
        if len(outs) >= 2 and {"logits", "pred_boxes"} <= names:
            return "detr"
        shape = outs[0].shape
        if len(shape) == 3 and shape[-1] == 6:
            return "end2end"
        if len(shape) == 3 and isinstance(shape[1], int) and isinstance(shape[2], int):
            if shape[1] > 4 and shape[2] > shape[1]:
                return "anchors"
        raise RuntimeError(
            "unsupported detector outputs "
            + ", ".join(f"{o.name}{o.shape}" for o in outs)
            + "; expected [1,N,6], logits+pred_boxes, or [1,4+C,A]"
        )

    def _raw_predictions(
        self, nchw: np.ndarray
    ) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        """Boxes (xyxy, letterboxed pixels), scores and class ids for one image."""
        with self.lock:
            outputs = self.session.run(None, {self.input_name: nchw})
        if self.layout == "end2end":
            p = outputs[0][0]
            return p[:, :4], p[:, 4], p[:, 5].astype(np.int64)
        if self.layout == "detr":
            by_name = dict(zip([o.name for o in self.session.get_outputs()], outputs))
            logits, boxes = by_name["logits"][0], by_name["pred_boxes"][0]
            probs = 1.0 / (1.0 + np.exp(-logits))
            cls = probs.argmax(1)
            return (
                cxcywh_to_xyxy(boxes * self.imgsz),
                probs[np.arange(len(cls)), cls],
                cls,
            )
        p = outputs[0][0].T
        cls = p[:, 4:].argmax(1)
        return cxcywh_to_xyxy(p[:, :4]), p[np.arange(len(cls)), 4 + cls], cls

    def detect(
        self, frame_bgr: np.ndarray, conf: float, allowed: set[int] | None, limit: int
    ):
        h, w = frame_bgr.shape[:2]
        nchw, ratio, pad_x, pad_y = letterbox(frame_bgr, self.imgsz)
        boxes, scores, cls = self._raw_predictions(nchw)
        mask = (scores >= conf) & (scores > 0)
        if allowed is not None:
            mask &= np.isin(cls, list(allowed))
        boxes, scores, cls = boxes[mask], scores[mask], cls[mask]
        if self.layout == "anchors" and len(scores):
            # Offset boxes per class so one NMS pass never suppresses across classes.
            shifted = boxes + (cls[:, None] * (self.imgsz + 1)).astype(boxes.dtype)
            keep = nms(shifted, scores, NMS_IOU)
            boxes, scores, cls = boxes[keep], scores[keep], cls[keep]
        order = scores.argsort()[::-1][:limit]
        detections = []
        for i in order:
            x1, y1, x2, y2 = boxes[i].astype(np.float64)
            x1 = float(np.clip((x1 - pad_x) / ratio, 0, w))
            x2 = float(np.clip((x2 - pad_x) / ratio, 0, w))
            y1 = float(np.clip((y1 - pad_y) / ratio, 0, h))
            y2 = float(np.clip((y2 - pad_y) / ratio, 0, h))
            idx = int(cls[i])
            detections.append(
                {
                    "label": (
                        self.labels[idx]
                        if 0 <= idx < len(self.labels)
                        else f"class_{idx}"
                    ),
                    "class_id": idx,
                    "score": round(float(scores[i]), 5),
                    "box": {
                        "x1": round(x1, 2),
                        "y1": round(y1, 2),
                        "x2": round(x2, 2),
                        "y2": round(y2, 2),
                    },
                }
            )
        return {"width": int(w), "height": int(h), "detections": detections}


class BadRequest(ValueError):
    pass


def handle_detect(detector: Detector, body: dict) -> dict:
    images = body.get("images")
    if isinstance(images, str):
        images = [images]
    if (
        not isinstance(images, list)
        or not images
        or not all(isinstance(i, str) for i in images)
    ):
        raise BadRequest("'images' must be a non-empty list of base64 strings")
    conf = float(body.get("confidence", 0.25))
    limit = int(body.get("max_detections", 100))
    allowed = None
    unresolved: list[str] = []
    requested = body.get("classes")
    if requested:
        allowed = set()
        for name in requested:
            label = resolve_class(str(name), detector.labels)
            if label is None:
                unresolved.append(str(name))
            else:
                allowed.add(detector.labels.index(label))
    t0 = time.perf_counter()
    data = []
    for index, encoded in enumerate(images):
        try:
            frame = decode_image(encoded)
        except (ValueError, binascii.Error, OSError) as e:
            raise BadRequest(f"images[{index}] is not a decodable image: {e}") from e
        result = detector.detect(frame, conf, allowed, limit)
        result["index"] = index
        data.append(result)
    return {
        "data": data,
        "unresolved_classes": unresolved,
        "inference_ms": round(1000 * (time.perf_counter() - t0), 2),
    }


def make_handler(detector: Detector):
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
                        "model": str(detector.onnx_path),
                        "layout": detector.layout,
                        "imgsz": detector.imgsz,
                        "classes": len(detector.labels),
                        "providers": detector.providers,
                    },
                )
            else:
                self._send(
                    404, {"error": {"message": "not found", "type": "not_found"}}
                )

        def do_POST(self):
            if self.path.rstrip("/") != "/detect":
                self._send(
                    404, {"error": {"message": "not found", "type": "not_found"}}
                )
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(length) or b"{}")
                if not isinstance(body, dict):
                    raise BadRequest("request body must be a JSON object")
                self._send(200, handle_detect(detector, body))
            except (BadRequest, ValueError, TypeError) as e:
                self._send(
                    400, {"error": {"message": str(e), "type": "invalid_request_error"}}
                )
            except Exception as e:  # noqa: BLE001
                log(f"detect failed: {e!r}")
                self._send(500, {"error": {"message": str(e), "type": "server_error"}})

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Ryzen AI NPU object detection server", allow_abbrev=False
    )
    parser.add_argument(
        "--model-path", required=True, help=".onnx file or directory holding one"
    )
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--device", choices=["npu", "cpu"], default="npu")
    parser.add_argument("--cache-dir", default=str(Path.cwd() / "ryzenai-vision-cache"))
    parser.add_argument(
        "--cache-key", default="", help="default: <model stem>_<content hash>"
    )
    parser.add_argument("--vaip-config", default=str(SCRIPT_DIR / "vaiep_config.json"))
    parser.add_argument(
        "--imgsz", type=int, default=640, help="input size for dynamic models"
    )
    parser.add_argument(
        "--status-file", default="", help="startup errors are written here"
    )
    args = parser.parse_args()

    try:
        detector = Detector(args)
        server = ThreadingHTTPServer((args.host, args.port), make_handler(detector))
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
