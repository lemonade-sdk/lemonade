from typing import Union, Dict
import numpy as np
import torch
import onnx
from turnkeyml.common import printing
import turnkeyml.common.filesystem as fs


class AnalysisException(Exception):
    """
    Indicates a failure during analysis
    """


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


def get_onnx_ops_list(onnx_model) -> Dict:
    """
    List unique ops found in the onnx model
    """
    onnx_ops_counter = {}
    try:
        model = onnx.load(onnx_model)
    except Exception as e:  # pylint: disable=broad-except
        printing.log_warning(f"Failed to get ONNX ops list from {onnx_model}: {str(e)}")
        return onnx_ops_counter
    for node in model.graph.node:  # pylint: disable=E1101
        onnx_ops_counter[node.op_type] = onnx_ops_counter.get(node.op_type, 0) + 1
    return onnx_ops_counter


def attribute_to_dict(attribute):
    """
    Helper function that returns a dictionary containing node attributes
    """
    attribute_dict = {}
    for field in ["f", "i", "s"]:
        if attribute.HasField(field):
            attribute_dict[attribute.name] = getattr(attribute, field)
            return attribute_dict
    if attribute.ints:
        attribute_dict[attribute.name] = list(attribute.ints)
    elif attribute.floats:
        attribute_dict[attribute.name] = list(attribute.floats)
    elif attribute.strings:
        attribute_dict[attribute.name] = list(attribute.strings)
    else:
        attribute_dict[attribute.name] = "unknown_type"
    return attribute_dict


def get_onnx_total_flops(onnx_model) -> Union[int, None]:
    """
    Calculate total number of FLOPs found in the onnx model.
    FLOP is defined as one floating-point operation. This distinguishes
    from multiply-accumulates (MACs) where FLOPs == 2 * MACs.
    """
    try:
        onnx.shape_inference.infer_shapes_path(
            model_path=onnx_model,
            output_path=onnx_model,
            strict_mode=True,
            data_prop=True,
        )
        model = onnx.load(onnx_model, load_external_data=False)
    except Exception as e:  # pylint: disable=broad-except
        printing.log_warning(f"Failed to get ONNX FLOPs from {onnx_model}: {str(e)}")
        return None

    # If the ONNX model contains one of the following unsupported ops, then this
    # function will return None since the FLOP total is expected to be incorrect
    unsupported_ops = [
        "Einsum",
        "RNN",
        "GRU",
        "DeformConv",
    ]

    total_flops = np.int64(0)
    for node in model.graph.node:  # pylint: disable=E1101
        input_tensors = {tensor.name: tensor for tensor in model.graph.input}
        output_tensors = {tensor.name: tensor for tensor in model.graph.output}
        value_tensors = {tensor.name: tensor for tensor in model.graph.value_info}
        init_tensors = {tensor.name: tensor for tensor in model.graph.initializer}

        # input_dims is a 2 dimensional array where the first dimension indexes inputs
        # and the second dimension indexes dimensions
        input_dims = []
        for input in node.input:
            input_dims.append([])
            if (
                input in input_tensors
                or input in value_tensors
                or input in output_tensors
            ):
                tensor = (
                    input_tensors.get(input)
                    or value_tensors.get(input)
                    or output_tensors.get(input)
                )
                input_dims[-1].extend(
                    [
                        np.int64(dim.dim_value)
                        for dim in tensor.type.tensor_type.shape.dim
                    ]
                )
            elif input in init_tensors:
                input_dims[-1].extend([dim for dim in init_tensors.get(input).dims])

        attributes = {}
        for attribute in node.attribute:
            attributes.update(attribute_to_dict(attribute))

        current_op_flops = 0

        if node.op_type in unsupported_ops:
            return None

        elif (
            node.op_type == "MatMul"
            or node.op_type == "MatMulInteger"
            or node.op_type == "QLinearMatMul"
        ):
            input_a = input_dims[0]
            input_b = (
                input_dims[3] if node.op_type == "QLinearMatMul" else input_dims[1]
            )
            current_op_flops = 2 * np.prod(input_a, dtype=np.int64) * input_b[-1]

        elif node.op_type == "Mul" or node.op_type == "Div" or node.op_type == "Add":
            current_op_flops = np.prod(input_dims[0], dtype=np.int64) + np.prod(
                input_dims[1], dtype=np.int64
            )

        elif node.op_type == "Gemm" or node.op_type == "QGemm":
            x_shape = input_dims[0]
            w_shape = input_dims[1] if node.op_type == "Gemm" else input_dims[3]
            mm_dims = [
                x_shape[0] if not attributes.get("transA", 0) else x_shape[1],
                x_shape[1] if not attributes.get("transA", 0) else x_shape[0],
                w_shape[1] if not attributes.get("transB", 0) else w_shape[0],
            ]
            current_op_flops = 2 * np.prod(mm_dims, dtype=np.int64)
            if len(mm_dims) == 3:  # if there is a bias input
                current_op_flops += np.prod(input_dims[2], dtype=np.int64)

        elif (
            node.op_type == "Conv"
            or node.op_type == "ConvInteger"
            or node.op_type == "QLinearConv"
            or node.op_type == "ConvTranspose"
        ):
            x_shape = input_dims[0]  # N, C, d1, ..., dn
            w_shape = (
                input_dims[3] if node.op_type == "QLinearConv" else input_dims[1]
            )  # M, C/group, k1, ..., kn. Note C and M are swapped for ConvTranspose

            has_bias = False  # Note, ConvInteger has no bias
            if node.op_type == "Conv" and len(input_dims) == 3:
                has_bias = True
            elif node.op_type == "QLinearConv" and len(input_dims) == 9:
                has_bias = True

            num_dims = len(x_shape) - 2
            strides = attributes.get("strides", [1] * num_dims)
            dilation = attributes.get("dilations", [1] * num_dims)
            kernel_shape = w_shape[2:]
            batch_size = x_shape[0]
            out_channels = w_shape[0]
            out_dims = [batch_size, out_channels]
            output_shape = attributes.get("output_shape", [])

            # If output_shape is given then we do not need to compute it ourselves
            # The output_shape attribute does not include batch_size or channels and
            # is only valid for ConvTranspose
            if output_shape:
                out_dims.extend(output_shape)
            else:
                auto_pad = attributes.get("auto_pad", "NOTSET".encode()).decode()
                # SAME expects padding so that the output_shape = CEIL(input_shape / stride)
                if auto_pad == "SAME_UPPER" or auto_pad == "SAME_LOWER":
                    out_dims.extend([x * s for x, s in zip(x_shape[2:], strides)])
                else:
                    # NOTSET means just use pads attribute
                    if auto_pad == "NOTSET":
                        pads = attributes.get("pads", [0] * num_dims * 2)
                    # VALID essentially means no padding
                    elif auto_pad == "VALID":
                        pads = [0] * num_dims * 2

                    for i in range(num_dims):
                        dim_in = x_shape[i + 2]

                        if node.op_type == "ConvTranspose":
                            out_dim = (
                                strides[i] * (dim_in - 1)
                                + ((kernel_shape[i] - 1) * dilation[i] + 1)
                                - pads[i]
                                - pads[i + num_dims]
                            )
                        else:
                            out_dim = (
                                dim_in
                                + pads[i]
                                + pads[i + num_dims]
                                - dilation[i] * (kernel_shape[i] - 1)
                                - 1
                            ) // strides[i] + 1

                        out_dims.append(out_dim)

            kernel_flops = np.prod(kernel_shape, dtype=np.int64) * w_shape[1]
            output_points = np.prod(out_dims, dtype=np.int64)
            bias_ops = output_points if has_bias else 0
            current_op_flops = 2 * kernel_flops * output_points + bias_ops

        elif node.op_type == "LSTM" or node.op_type == "DynamicQuantizeLSTM":
            hidden_size = attributes.get("hidden_size")
            direction = (
                2 if attributes.get("direction") == "bidirectional".encode() else 1
            )
            bias_ops = 0 if not input_dims[3] else input_dims[3][1]
            seq_length, batch_size, input_dim = input_dims[0]
            num_gates = 4
            gate_input_flops = np.int64(2) * input_dim * hidden_size
            gate_hid_flops = np.int64(2) * hidden_size * hidden_size
            unit_flops = num_gates * (gate_input_flops + gate_hid_flops) + bias_ops
            current_op_flops = batch_size * seq_length * direction * unit_flops

        total_flops += current_op_flops

    return int(total_flops)


def populate_onnx_model_info(onnx_model) -> Dict:
    """
    Read the model metadata to populate IR, Opset and model size
    """
    result_dict = {
        "ir_version": None,
        "opset": None,
        "size on disk (KiB)": None,
    }
    try:
        model = onnx.load(onnx_model)
    except Exception as e:  # pylint: disable=broad-except
        printing.log_warning(f"Failed to get ONNX ops list from {onnx_model}: {str(e)}")
        result_dict.update({"error": "ONNX model analysis failed"})
        return result_dict
    # pylint: disable=E1101
    result_dict.update(
        {
            "ir_version": getattr(model, "ir_version", None),
            "opset": getattr(model.opset_import[0], "version", None),
        }
    )
    try:
        result_dict.update(
            {
                "size on disk (KiB)": round(
                    model.SerializeToString().__sizeof__() / 1024, 4
                ),
            }
        )
    except ValueError:
        # Models >2GB on disk cannot have their model size measured this
        # way and will throw a ValueError https://github.com/onnx/turnkeyml/issues/41
        pass

    return result_dict


def onnx_input_dimensions(onnx_model) -> Dict:
    """
    Read model input dimensions
    """
    input_shape = {}
    try:
        model = onnx.load(onnx_model)
    except Exception as e:  # pylint: disable=broad-except
        printing.log_warning(f"Failed to get ONNX ops list from {onnx_model}: {str(e)}")
        return input_shape
    for input in model.graph.input:  # pylint: disable=E1101
        shape = str(input.type.tensor_type.shape.dim)
        input_shape[input.name] = [int(s) for s in shape.split() if s.isdigit()]
    return input_shape


def analyze_onnx(build_name: str, cache_dir: str, stats: fs.Stats):
    # ONNX stats that we want to save into the build's turnkey_stats.yaml file
    # so that they can be easily accessed by the report command later
    if fs.Keys.ONNX_FILE in stats.evaluation_stats.keys():
        # Just in case the ONNX file was generated on a different machine:
        # strip the state's cache dir, then prepend the current cache dir
        final_onnx_file = fs.rebase_cache_dir(
            stats.evaluation_stats[fs.Keys.ONNX_FILE],
            build_name,
            cache_dir,
        )

        onnx_ops_counter = get_onnx_ops_list(final_onnx_file)
        onnx_total_flops = get_onnx_total_flops(final_onnx_file)
        onnx_model_info = populate_onnx_model_info(final_onnx_file)
        input_dimensions = onnx_input_dimensions(final_onnx_file)

        stats.save_stat(
            fs.Keys.ONNX_OPS_COUNTER,
            onnx_ops_counter,
        )
        stats.save_stat(
            fs.Keys.ONNX_TOTAL_FLOPS,
            onnx_total_flops,
        )
        stats.save_stat(
            fs.Keys.ONNX_MODEL_INFO,
            onnx_model_info,
        )
        stats.save_stat(
            fs.Keys.ONNX_INPUT_DIMENSIONS,
            input_dimensions,
        )
