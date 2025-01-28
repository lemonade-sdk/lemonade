# CLI PyTorch Examples

You can try out `turnkey` with PyTorch models, via the `discover` tool, like so:

```bash
cd turnkeyml/examples/turnkey/cli/scripts
turnkey -i hello_world.py discover export-pytorch
```
This will discover the model within `hello_world.py` and export it to ONNX.

See the docstring in each script for more information.
- `hello_world.py`: example with a single-layer PyTorch model.
- `max_depth.py`: example with a multi-layer PyTorch model, to try out the `discover --max-depth DEPTH` option.
- `multiple_invocations.py`: example where a model is invoked multiple times with different input shapes. Discovery treats each unique invocation separately, since exporting them to ONNX will result in different ONNX files.
`two_models.py`: example where two different models are discovered.
