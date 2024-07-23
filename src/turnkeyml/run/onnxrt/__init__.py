from .runtime import OnnxRT

implements = {
    "runtimes": {
        "ort": {
            "RuntimeClass": OnnxRT,
            "supported_devices": {"x86"},
        }
    }
}
