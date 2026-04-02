### Bundling Backends

`lemond` can download backends such as `llama-server` on your behalf, or it can utilize backends that are already part of your app. You can also download backends at packaging time, install time, or runtime.

#### Using Pre-Existing Backends

You can provide `lemond` the path to pre-existing backends with the following settings. This will cause `lemond` to use your custom backend binaries instead of downloading its own.

```
# Start lemond to update configuration
lemond ./

# Set the llama-server vulkan binary path
lemonade config set llamacpp.vulkan_bin /path/to/bins

# Works for llamacpp.rocm_bin and llamacpp.cpu_bin as well

# Set the sdcpp binary path
lemonade config set
