from .runtime import TensorRT


implements = {
    "runtimes": {
        "trt": {
            "RuntimeClass": TensorRT,
            "supported_devices": {"nvidia"},
        }
    }
}
