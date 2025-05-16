import numpy as np
import torch
import onnx


def count_parameters(model: torch.nn.Module) -> int:
    """
    Returns the number of parameters of a given model
    """
    if isinstance(model, (torch.nn.Module, torch.jit.ScriptModule)):
        return sum([parameter.numel() for _, parameter in model.named_parameters()])
    elif isinstance(model, str) and model.endswith(".onnx"):
        onnx_model = onnx.load(model)
        return int(
            sum(
                np.prod(tensor.dims, dtype=np.int64)
                for tensor in onnx_model.graph.initializer
                if tensor.name not in onnx_model.graph.input
            )
        )
    else:
        return None


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
