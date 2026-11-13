"""Adapt Lemonade's shared checkpoints to the installed Strata release."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys


def load_setup(control):
    os.environ["STRATA_CONTROL_INSTALL"] = "1"
    os.environ["STRATA_INSTALL_DIR"] = str(control.parent / "preparation")
    sys.path.insert(0, str(control))
    spec = importlib.util.spec_from_file_location(
        "lemonade_strata_setup", control / "setup.py"
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def run_tool(control, name, *args):
    subprocess.run(
        [sys.executable, "-E", "-s", str(control / name), *map(str, args)], check=True
    )


def ple_shard(control, files):
    sys.path.insert(0, str(control / "tools"))
    from gguf_reader import GGUFFile

    for path in files:
        if any(
            tensor.name == "per_layer_token_embd.weight"
            for tensor in GGUFFile(path).tensors
        ):
            return path
    raise ValueError(f"No per_layer_token_embd.weight tensor in {files[0]}'s shards")


def prepare_mtp(setup, control, cache, offline=False):
    runtime = cache / "rt"
    marker = cache / ".lemonade-ready"
    files = ("dense.bin", "dense.txt", "experts.bin", "draft_vocab.bin")
    if not marker.is_file() or not all(
        (runtime / name).is_file() and (runtime / name).stat().st_size > 0
        for name in files
    ):
        if offline:
            raise RuntimeError(
                "Strata MTP is not prepared while offline; prepare it online first or use --mtp off"
            )
        cache.mkdir(parents=True, exist_ok=True)
        print(
            "Preparing the shared Strata MTP draft layer (about 5.2 GB to download).",
            flush=True,
        )
        setup.fetch_mtp(cache, os.environ.copy())
        os.environ["STRATA_GGUF_PY"] = str(setup.get_llama_cpp() / "gguf-py")
        gguf = cache / "mtp-q2_0.gguf"
        run_tool(
            control,
            "tools/mtp_pack.py",
            "--src",
            cache,
            "--experts",
            "q2_0",
            "--out",
            gguf,
        )
        run_tool(control, "tools/mtp_rt.py", "--gguf", gguf, "--out", runtime)
        setup.refresh_draft_vocab(runtime)
        if not all(
            (runtime / name).is_file() and (runtime / name).stat().st_size > 0
            for name in files
        ):
            raise RuntimeError(
                "Strata MTP preparation did not produce all runtime files"
            )
        marker.touch()
    return runtime


def prepare(setup, control, request):
    selection = request["selection"]
    family, model = selection["family"], selection["model"]
    backend, context = request["backend"], request["context"]
    if family not in setup.FAMILIES and family not in setup.OWN_PATH_FAMILIES:
        raise ValueError(f"Unsupported Qwen engine family: {family}")
    if backend not in selection["backends"]:
        raise ValueError(f"{family}/{model} does not support {backend}")
    paths = {key: Path(value) for key, value in request["paths"].items()}
    extra = [
        part
        for arg in request["args"]
        for part in (arg.split("=", 1) if arg.startswith("--") else [arg])
    ]
    source, output = paths["main"], Path(request["output"])
    pack = Path(request["cache"])
    pack.mkdir(parents=True, exist_ok=True)
    marker = pack / ".lemonade-ready"
    cfg = {
        "exe": request["executable"],
        "cwd": str(control),
        "model_name": request["name"],
        "log": str(output / "engine.log"),
        "lib_dirs": [
            str(Path(request["executable"]).parent),
            str(Path(request["executable"]).parent / "rocm" / "bin"),
        ],
        "backend": {"rocm": "hip"}.get(backend, backend),
        "parallel": request["parallel"],
    }
    if family in setup.OWN_PATH_FAMILIES:
        if not marker.is_file() or not (pack / "tokenizer" / "vocab.json").is_file():
            run_tool(
                control, "tools/strata_tokenizer.py", "--gguf", source, "--out", pack
            )
            marker.touch()
        cfg.update(
            arch=setup.OWN_PATH_FAMILIES[family]["arch"],
            tokenizer=str(pack / "tokenizer"),
        )
        args = [
            "--native",
            str(source),
            "--max-context",
            str(context),
            "--prefill",
            "auto",
            "--mtp",
            "2",
        ]
    else:
        definition = setup.FAMILIES[family]
        shards = [source]
        if not definition.get("ready"):
            shards += [
                paths[f"shard_{i}"]
                for i in range(2, setup.model_shards(definition, model) + 1)
            ]
            ple = ple_shard(control, shards)
        if definition.get("ready"):
            pack = source
            args = [
                "--pack",
                str(pack),
                "--native",
                str(paths["dense"]),
                "--native-dense-gguf",
                str(paths["dense"]),
                "--ple-gguf",
                str(paths["ple"]),
                "--embd-gguf",
                str(paths["embd"]),
            ]
        else:
            native = pack / "model-metal.gguf" if backend == "metal" else source
            if backend == "metal" and not native.is_file():
                run_tool(
                    control,
                    "metal/tools/metal_repack.py",
                    "--shard1",
                    source,
                    "--out",
                    native,
                    "--verify",
                )
            if (
                not marker.is_file()
                or not (pack / "tokenizer" / "vocab.json").is_file()
            ):
                os.environ["STRATA_GGUF_PY"] = str(setup.get_llama_cpp() / "gguf-py")
                run_tool(
                    control,
                    "tools/iq_pack.py",
                    "--gguf",
                    native,
                    "--out",
                    pack,
                    *definition.get("pack_args", []),
                )
                marker.touch()
            args = ["--pack", str(pack), "--native", str(native)]
            if backend == "metal":
                args += ["--native-dense-gguf", str(native)]
            if len(shards) <= 2 or backend == "metal":
                args += ["--ple-gguf", str(ple)]
        cfg["tokenizer"] = str(pack / "tokenizer")
        args += [
            "--expert-profile",
            str(control / "data" / definition.get("profile", "expert-profile.bin")),
            "--expert-cache",
            "64" if backend == "metal" else "auto",
            "--prefill",
            "auto",
            "--spec",
            "2" if backend == "metal" else "4",
            "--max-context",
            str(context),
        ]
        if backend == "metal":
            args += ["--suffix-draft", "0", "--mmap-experts"]
            cfg["env"] = {"STRATA_METAL_STREAM": "1"}
        else:
            args += ["--spec-min-p", "0.5"]
        if backend != "metal":
            parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
            parser.add_argument("--kv", default="int8" if context > 8192 else "fp16")
            kv, _ = parser.parse_known_args(extra)
            args += ["--kv", kv.kv]
            if setup.kv_streaming_wanted(model, context, kv.kv, setup.ram_gb()):
                args += ["--kv-resident", "32768"]
        if backend == "sycl":
            from strata_cli.launch.adopt import intel_driver

            args += (
                ["--ple-io", "ram"]
                if intel_driver() == "i915"
                else ["--stream-experts"]
            )
            args += ["--vram-reserve-mib", "1024" if context <= 32768 else "2048"]
        parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
        parser.add_argument("--mtp", default="auto")
        draft, extra = parser.parse_known_args(extra)
        if draft.mtp not in ("auto", "off"):
            args += ["--mtp", draft.mtp]
        elif backend != "metal" and draft.mtp == "auto":
            parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
            parser.add_argument("--spec", type=int, default=4)
            speculation, _ = parser.parse_known_args(extra)
            if speculation.spec > 0:
                args += [
                    "--mtp",
                    str(
                        prepare_mtp(
                            setup,
                            control,
                            Path(request["mtp_cache"]),
                            offline=request["offline"],
                        )
                    ),
                ]
    if "--chunk" in extra:
        index = args.index("--prefill")
        del args[index : index + 2]
    cfg.update(args=args, user_args=extra)
    Path(request["config"]).write_text(
        json.dumps(cfg, indent=2) + "\n", encoding="utf-8"
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--control", type=Path, required=True)
    parser.add_argument("--request", type=Path, required=True)
    options = parser.parse_args()
    control = options.control.resolve()
    prepare(
        load_setup(control),
        control,
        json.loads(options.request.read_text(encoding="utf-8")),
    )


if __name__ == "__main__":
    main()
