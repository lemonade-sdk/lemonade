# CLI Example Plugins

This directory contains plugins that can be installed to demonstrate how turnkeyml can be extended via the plugin interface:
- `example_rt`: Example of a runtime plugin. Install with `pip install -e example_rt` to add the `example-rt` runtime to your turnkey CLI.
- `example_tool`: Example of a Tool plugin. Install with `pip install -e example_tool` to add the `example-tool` Tool to your turnkey CLI.
- `example_combined`: Example of a plugin that includes both a sequence and a runtime. Install with `pip install -e example_combined` to add the `example-combined-rt` runtime and `example-combined-tool` Tool to your turnkey CLI.

See the [plugins contribution guide](https://github.com/onnx/turnkeyml/blob/main/docs/contribute.md#contributing-a-plugin) for information about how to create plugins.