from .runtime import CombinedExampleRT, combined_rt_name
from .tool import CombinedExampleTool

implements = {
    "runtimes": {
        combined_rt_name: {
            "RuntimeClass": CombinedExampleRT,
            "supported_devices": {
                "x86": {},
                "example_family": {"part1": ["config1", "config2"]},
            },
        }
    },
    "tools": [CombinedExampleTool],
}
