"""
FastAPI service for Semantic Router

Provides HTTP endpoints for routing requests from the C++ server.
Start with: python -m semantic_router.service --config path/to/config.yaml
"""

import argparse
import logging
import sys
from typing import Dict, Any, Optional
from pathlib import Path

try:
    from fastapi import FastAPI, HTTPException
    from fastapi.responses import JSONResponse
    from pydantic import BaseModel
    import uvicorn
except ImportError:
    print("FastAPI and uvicorn required. Install with: pip install fastapi uvicorn")
    sys.exit(1)

from .simple_router import (
    route,
    route_with_metadata,
    validate_config,
    load_config,
    get_default_config,
    get_sample_config,
)

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = FastAPI(
    title="Lemonade Semantic Router",
    description="Intelligent prompt-based routing between local and cloud models",
    version="0.1.0",
)

# Enable CORS for browser-based config updates
try:
    from fastapi.middleware.cors import CORSMiddleware

    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
    )
except ImportError:
    logger.warning("CORSMiddleware not available, browser config updates may fail")

# Global config (loaded on startup)
_config: Dict[str, Any] = {}
_config_path: Optional[str] = None


class RouteRequest(BaseModel):
    """Request body for /route endpoint"""

    prompt: str
    model: Optional[str] = None  # Original model from request (for logging)


class RouteResponse(BaseModel):
    """Response from /route endpoint"""

    action: str  # "allow", "block", "redirect"
    model: Optional[str] = None
    reason: str = ""
    latency_ms: float = 0.0


class ConfigRequest(BaseModel):
    """Request body for /config endpoint"""

    config_yaml: str


class HealthResponse(BaseModel):
    """Response from /health endpoint"""

    status: str
    config_loaded: bool
    version: str = "0.1.0"


@app.get("/health", response_model=HealthResponse)
async def health():
    """Health check endpoint"""
    return HealthResponse(
        status="healthy" if _config else "no_config",
        config_loaded=bool(_config),
    )


@app.post("/route", response_model=RouteResponse)
async def route_request(request: RouteRequest):
    """
    Route a prompt to the appropriate model

    Returns routing decision with action and target model.
    """
    if not _config:
        raise HTTPException(status_code=503, detail="No routing config loaded")

    try:
        result = route_with_metadata(request.prompt, _config)
        return RouteResponse(
            action=result["action"],
            model=result.get("model"),
            reason=result.get("reason", ""),
            latency_ms=result.get("latency_ms", 0.0),
        )
    except Exception as e:
        logger.error(f"Routing error: {e}")
        # Fail-open: allow request if routing fails
        if _config.get("settings", {}).get("fail_open", True):
            return RouteResponse(
                action="allow",
                model=None,
                reason=f"Routing error (fail-open): {str(e)}",
            )
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/route/detailed")
async def route_request_detailed(request: RouteRequest):
    """
    Route a prompt with full signal details

    Returns complete routing decision including all signal results.
    """
    if not _config:
        raise HTTPException(status_code=503, detail="No routing config loaded")

    try:
        result = route_with_metadata(request.prompt, _config)
        return JSONResponse(content=result)
    except Exception as e:
        logger.error(f"Routing error: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/config")
async def get_config():
    """Get current routing config"""
    return JSONResponse(
        content={
            "config_path": _config_path,
            "config": _config,
        }
    )


@app.post("/config")
async def update_config(request: ConfigRequest):
    """
    Update routing config from YAML

    Validates config before applying.
    """
    global _config

    try:
        import yaml

        new_config = yaml.safe_load(request.config_yaml)
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Invalid YAML: {str(e)}")

    is_valid, errors = validate_config(new_config)
    if not is_valid:
        raise HTTPException(status_code=400, detail={"errors": errors})

    _config = new_config
    logger.info("Config updated successfully")

    return JSONResponse(
        content={
            "status": "ok",
            "models": len(new_config.get("models", [])),
            "signals_enabled": {
                "jailbreak": new_config.get("signals", {})
                .get("jailbreak", {})
                .get("enabled", True),
                "pii": new_config.get("signals", {})
                .get("pii", {})
                .get("enabled", True),
                "keywords": len(new_config.get("signals", {}).get("keywords", {})),
                "complexity": new_config.get("signals", {})
                .get("complexity", {})
                .get("enabled", True),
            },
        }
    )


@app.post("/config/validate")
async def validate_config_endpoint(request: ConfigRequest):
    """Validate config YAML without applying"""
    try:
        import yaml

        config = yaml.safe_load(request.config_yaml)
    except Exception as e:
        return JSONResponse(
            content={
                "valid": False,
                "errors": [f"Invalid YAML: {str(e)}"],
            }
        )

    is_valid, errors = validate_config(config)
    return JSONResponse(
        content={
            "valid": is_valid,
            "errors": errors,
        }
    )


@app.get("/config/sample")
async def get_sample_config_endpoint():
    """Get sample config YAML"""
    return JSONResponse(
        content={
            "sample": get_sample_config(),
        }
    )


@app.post("/config/reload")
async def reload_config():
    """Reload config from disk (if config_path was set)"""
    global _config

    if not _config_path:
        raise HTTPException(status_code=400, detail="No config path set")

    try:
        _config = load_config(_config_path)
        logger.info(f"Config reloaded from {_config_path}")
        return JSONResponse(content={"status": "ok", "path": _config_path})
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


def main():
    """Main entry point for CLI"""
    global _config, _config_path

    parser = argparse.ArgumentParser(description="Lemonade Semantic Router Service")
    parser.add_argument(
        "--config",
        "-c",
        type=str,
        help="Path to YAML config file",
    )
    parser.add_argument(
        "--host",
        type=str,
        default="127.0.0.1",
        help="Host to bind to (default: 127.0.0.1)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=8765,
        help="Port to listen on (default: 8765)",
    )
    parser.add_argument(
        "--log-level",
        type=str,
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Log level",
    )

    args = parser.parse_args()

    logging.getLogger().setLevel(args.log_level)

    # Load config if provided
    if args.config:
        config_path = Path(args.config)
        if not config_path.exists():
            logger.error(f"Config file not found: {args.config}")
            sys.exit(1)

        _config_path = str(config_path)
        _config = load_config(_config_path)
        logger.info(f"Loaded config from {_config_path}")

        is_valid, errors = validate_config(_config)
        if not is_valid:
            logger.error(f"Config validation failed: {errors}")
            sys.exit(1)
    else:
        logger.warning("No config file provided, starting with empty config")
        _config = get_default_config()

    # Pre-load MMBERT models so first request doesn't timeout
    logger.info("Pre-loading MMBERT models (this may take a moment)...")
    try:
        from .strategies.embedding import (
            detect_jailbreak_mmbert,
            detect_pii_mmbert,
            score_complexity_mmbert,
        )

        # Dummy call to trigger model loading
        dummy_messages = [{"role": "user", "content": "hello"}]
        detect_jailbreak_mmbert(dummy_messages, threshold=0.99)
        detect_pii_mmbert(dummy_messages, threshold=0.99)
        score_complexity_mmbert(dummy_messages)  # Pre-load embedding model too
        logger.info("MMBERT models loaded successfully")
    except Exception as e:
        logger.warning(f"Failed to pre-load MMBERT models: {e}")

    logger.info(f"Starting semantic router on {args.host}:{args.port}")
    uvicorn.run(app, host=args.host, port=args.port, log_level=args.log_level.lower())


if __name__ == "__main__":
    main()
