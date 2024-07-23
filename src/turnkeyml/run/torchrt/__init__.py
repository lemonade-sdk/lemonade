from .runtime import TorchRT

implements = {
    "runtimes": {
        "torch-eager": {
            "RuntimeClass": TorchRT,
            "supported_devices": {"x86"},
        },
        "torch-compiled": {
            "RuntimeClass": TorchRT,
            "supported_devices": {"x86"},
        },
    }
}
