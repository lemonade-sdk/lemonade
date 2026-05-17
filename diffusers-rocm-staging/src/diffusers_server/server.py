"""
diffusers-server: a thin FastAPI wrapper around Hugging Face Diffusers that
exposes OpenAI-compatible image-generation endpoints.

Designed to be launched as a subprocess by Lemonade (the same pattern as
vllm-server, llama-server, sd-server). Exactly one model is served per
process; multi-model serving is handled by Lemonade's router.

Endpoints:
  GET  /health                  liveness probe (also reports loaded model)
  POST /v1/images/generations   OpenAI Images API
  POST /v1/images/edits         OpenAI Images Edits (if pipeline supports it)
  POST /v1/images/variations    OpenAI Images Variations (if pipeline supports it)

CLI:
  diffusers-server --model HF_ID [--port N] [--host H]
                   [--served-model-name NAME] [--dtype bf16|fp16|fp32]
                   [--pipeline-class CLASS] [--variant VARIANT]
"""

import argparse
import base64
import io
import logging
import os
import sys
import time
from typing import Any, Optional

import torch
import uvicorn
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field

logger = logging.getLogger("diffusers-server")

_state: dict[str, Any] = {
    "pipe": None,
    "model_id": None,
    "served_model_name": None,
    "device": None,
    "dtype": None,
}


class ImageRequest(BaseModel):
    model: str
    prompt: str
    n: int = 1
    size: str = "1024x1024"
    response_format: str = "b64_json"
    # Diffusers extensions (passed through when present)
    num_inference_steps: Optional[int] = None
    guidance_scale: Optional[float] = None
    negative_prompt: Optional[str] = None
    seed: Optional[int] = None


def _parse_dtype(name: str) -> torch.dtype:
    return {
        "fp16": torch.float16,
        "bf16": torch.bfloat16,
        "fp32": torch.float32,
    }[name]


def _parse_size(size: str) -> tuple[int, int]:
    w, h = size.lower().split("x")
    return int(w), int(h)


def _pick_device() -> str:
    # ROCm exposes the HIP device through the "cuda" PyTorch namespace.
    if torch.cuda.is_available():
        return "cuda"
    return "cpu"


def _resolve_pipeline_class(name: Optional[str]):
    import diffusers as _d

    if not name:
        return _d.DiffusionPipeline
    cls = getattr(_d, name, None)
    if cls is None:
        raise ValueError(f"Pipeline class '{name}' not found in diffusers")
    return cls


def load_pipeline(
    model_id: str,
    dtype: torch.dtype,
    device: str,
    pipeline_class: Optional[str],
    variant: Optional[str],
):
    cls = _resolve_pipeline_class(pipeline_class)
    kwargs: dict[str, Any] = {"torch_dtype": dtype}
    if variant:
        kwargs["variant"] = variant
    logger.info("Loading %s from %s (dtype=%s, variant=%s)", cls.__name__, model_id, dtype, variant)
    pipe = cls.from_pretrained(model_id, **kwargs)
    pipe = pipe.to(device)
    return pipe


def _images_to_b64(images) -> list[dict[str, str]]:
    data = []
    for img in images:
        buf = io.BytesIO()
        img.save(buf, format="PNG")
        data.append({"b64_json": base64.b64encode(buf.getvalue()).decode("ascii")})
    return data


def create_app() -> FastAPI:
    app = FastAPI(title="diffusers-server", version="0.1.0")

    @app.get("/health")
    async def health():
        return {
            "status": "ok",
            "model": _state["served_model_name"],
            "device": _state["device"],
        }

    @app.post("/v1/images/generations")
    async def generations(req: ImageRequest):
        pipe = _state["pipe"]
        if pipe is None:
            raise HTTPException(503, "Model not loaded")
        if req.model != _state["served_model_name"]:
            raise HTTPException(
                404,
                f"Model '{req.model}' not served; this server is serving "
                f"'{_state['served_model_name']}'",
            )

        w, h = _parse_size(req.size)
        kwargs: dict[str, Any] = {
            "prompt": req.prompt,
            "num_images_per_prompt": req.n,
            "width": w,
            "height": h,
        }
        if req.num_inference_steps is not None:
            kwargs["num_inference_steps"] = req.num_inference_steps
        if req.guidance_scale is not None:
            kwargs["guidance_scale"] = req.guidance_scale
        if req.negative_prompt is not None:
            kwargs["negative_prompt"] = req.negative_prompt
        if req.seed is not None:
            kwargs["generator"] = torch.Generator(device=_state["device"]).manual_seed(req.seed)

        with torch.inference_mode():
            result = pipe(**kwargs)

        return {"created": int(time.time()), "data": _images_to_b64(result.images)}

    return app


def main():
    parser = argparse.ArgumentParser(prog="diffusers-server")
    parser.add_argument("--model", required=True, help="Hugging Face model id (e.g. Efficient-Large-Model/Sana_1600M_1024px_diffusers)")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--served-model-name", default=None,
                        help="Name to advertise in /v1/images responses; defaults to --model")
    parser.add_argument("--dtype", default="bf16", choices=["fp16", "bf16", "fp32"])
    parser.add_argument("--pipeline-class", default=None,
                        help="Override diffusers pipeline class (e.g. SanaPipeline). "
                             "Default: DiffusionPipeline auto-dispatch.")
    parser.add_argument("--variant", default=None,
                        help="Weight variant to load (e.g. fp16). Optional.")
    parser.add_argument("--log-level", default="info",
                        choices=["debug", "info", "warning", "error"])
    args, unknown = parser.parse_known_args()
    if unknown:
        logger.warning("Ignoring unknown args: %s", unknown)

    logging.basicConfig(
        level=args.log_level.upper(),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    device = _pick_device()
    dtype = _parse_dtype(args.dtype)
    served = args.served_model_name or args.model

    logger.info("Device: %s | dtype: %s", device, dtype)
    logger.info("Loading model: %s (served as %s)", args.model, served)

    _state["pipe"] = load_pipeline(args.model, dtype, device, args.pipeline_class, args.variant)
    _state["model_id"] = args.model
    _state["served_model_name"] = served
    _state["device"] = device
    _state["dtype"] = str(dtype)

    logger.info("Model loaded; starting HTTP server on %s:%d", args.host, args.port)
    app = create_app()
    uvicorn.run(app, host=args.host, port=args.port, log_level=args.log_level)


if __name__ == "__main__":
    main()
