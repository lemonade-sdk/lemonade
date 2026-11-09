"""Adapt Lemonade's shared checkpoints to the installed Strata release."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from urllib.parse import urlparse


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


def checkpoint(url, filename):
    parts = urlparse(url).path.strip("/").split("/")
    if len(parts) < 4 or parts[2] != "resolve":
        raise ValueError(f"Unsupported Strata model URL: {url}")
    return "/".join(parts[:2]) + ":" + "/".join([*parts[4:], filename])


def revision(url):
    value = urlparse(url).path.strip("/").split("/")[3]
    return "" if value == "main" else value


def catalog(setup):
    from strata_control import mac_fit

    rows = {}
    for family, definition in setup.FAMILIES.items():
        for model, detail in setup.MODELS.items():
            if family not in detail.get("families", ("qwen", "swift")) or detail.get(
                "prepare"
            ):
                continue
            ready = bool(definition.get("ready"))
            stem = {
                "qwen": "Qwen3.8-Flash-Next",
                "swift": "Swift-1.5",
                "coder": "Qwen3.8-Flash-Next-Coder",
                "unsloth": "Qwen3.8-Flash-Next-Unsloth",
                "gyro": "Qwen3.8-Flash-Next",
                "nvfp4": "Qwen3.8-Flash-Next",
            }[family]
            name = f"{stem}-{model}-Strata"
            sources = (
                {"main": detail["repo"] + ":pack"}
                if ready
                else {
                    "main": checkpoint(
                        setup.model_hf(definition, model),
                        setup.model_file(definition, model, 1),
                    )
                }
            )
            if ready:
                for filename, (role, *_rest) in setup.NVFP4_REPOS[detail["repo"]][
                    "files"
                ].items():
                    if role in ("dense", "ple", "embd"):
                        repo = (
                            setup.NVFP4_GPTQ_REPO
                            if role in ("ple", "embd")
                            else detail["repo"]
                        )
                        sources[role] = repo + ":" + filename
            else:
                sources.update(
                    {
                        f"shard_{i}": checkpoint(
                            setup.model_hf(definition, model),
                            setup.model_file(definition, model, i),
                        )
                        for i in range(2, setup.model_shards(definition, model) + 1)
                    }
                )
            backends = ["cuda"] if ready else ["cuda", "rocm", "sycl"]
            if family in mac_fit.APP_FAMILIES and model in mac_fit.app_sizes(family):
                backends.append("metal")
            rows[name] = {
                "checkpoints": sources,
                "recipe": "strata",
                "suggested": True,
                "labels": ["chat", "reasoning", "tool-calling"],
                "size": detail["download_gb"],
                "min_resident_gb": detail["ram_gb"],
                "strata": {
                    "family": family,
                    "model": model,
                    "format": "ready" if ready else "gguf",
                    "backends": backends,
                    "revisions": {
                        key: (
                            setup.NVFP4_REPOS[sources[key].split(":")[0]]["revision"]
                            if ready
                            else revision(setup.model_hf(definition, model))
                        )
                        for key in sources
                    },
                },
            }
    for family, definition in setup.OWN_PATH_FAMILIES.items():
        for model, detail in definition["models"].items():
            filename = definition["file"].format(q=model)
            name = (
                "Qwen3.6-35B-A3B-Strata"
                if model == "UD-Q4_K_XL"
                else f"{definition['title']}-{model}-Strata"
            )
            rows[name] = {
                "checkpoint": checkpoint(definition["hf"], filename),
                "recipe": "strata",
                "suggested": True,
                "labels": ["chat", "reasoning", "tool-calling", "mtp"],
                "size": round(definition["sha256"][filename][0] / 1e9, 1),
                "min_resident_gb": detail["ram_gb"],
                "strata": {
                    "family": family,
                    "model": model,
                    "format": "gguf",
                    "backends": ["cuda", "rocm"],
                    "revisions": {"main": revision(definition["hf"])},
                },
            }
    return rows


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


def update_catalog(text, rows):
    decoder = json.JSONDecoder()
    position = text.index("{") + 1
    replacements = []
    remaining = dict(rows)
    while True:
        while text[position].isspace() or text[position] == ",":
            position += 1
        if text[position] == "}":
            break
        name, position = decoder.raw_decode(text, position)
        position = text.index(":", position) + 1
        while text[position].isspace():
            position += 1
        start = position
        _, position = decoder.raw_decode(text, position)
        if name in remaining:
            replacements.append(
                (
                    start,
                    position,
                    json.dumps(remaining.pop(name), indent=4).replace("\n", "\n    "),
                )
            )
    if remaining:
        additions = json.dumps(remaining, indent=4)[1:-1].strip("\n")
        replacements.append((position, position, ",\n" + additions + "\n"))
        start, end, value = replacements[-1]
        while start > 0 and text[start - 1].isspace():
            start -= 1
        replacements[-1] = (start, end, value)
    for start, end, value in reversed(replacements):
        text = text[:start] + value + text[end:]
    return text


def prepare(setup, control, request):
    selection = request["selection"]
    family, model = selection["family"], selection["model"]
    backend, context = request["backend"], request["context"]
    if family not in setup.FAMILIES and family not in setup.OWN_PATH_FAMILIES:
        raise ValueError(f"Unsupported Qwen engine family: {family}")
    if backend not in selection["backends"]:
        raise ValueError(f"{family}/{model} does not support {backend}")
    paths = {key: Path(value) for key, value in request["paths"].items()}
    extra = request["args"]
    source, output = paths["main"], Path(request["output"])
    output.mkdir(parents=True, exist_ok=True)
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
        "host": "127.0.0.1",
        "open_browser": False,
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
        definition, detail = setup.FAMILIES[family], setup.MODELS[model]
        shards = [source]
        if not definition.get("ready"):
            shards += [
                paths[f"shard_{i}"]
                for i in range(2, setup.model_shards(definition, model) + 1)
            ]
            ple = ple_shard(control, shards)
        if definition.get("ready"):
            roles = paths
            pack = source
            args = [
                "--pack",
                str(pack),
                "--native",
                str(roles["dense"]),
                "--native-dense-gguf",
                str(roles["dense"]),
                "--ple-gguf",
                str(roles["ple"]),
                "--embd-gguf",
                str(roles["embd"]),
            ]
        elif backend == "metal":
            native = pack / "model-metal.gguf"
            if not native.is_file():
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
            args = [
                "--pack",
                str(pack),
                "--native",
                str(native),
                "--ple-gguf",
                str(ple),
                "--mmap-experts",
            ]
            cfg["env"] = {"STRATA_METAL_STREAM": "1"}
        else:
            if (
                not marker.is_file()
                or not (pack / "tokenizer" / "vocab.json").is_file()
            ):
                os.environ["STRATA_GGUF_PY"] = str(setup.get_llama_cpp() / "gguf-py")
                run_tool(
                    control,
                    "tools/iq_pack.py",
                    "--gguf",
                    source,
                    "--out",
                    pack,
                    *definition.get("pack_args", []),
                )
                marker.touch()
            args = ["--pack", str(pack), "--native", str(source)]
            if len(shards) <= 2:
                args += ["--ple-gguf", str(ple)]
        cfg["tokenizer"] = str(pack / "tokenizer")
        args += [
            "--expert-profile",
            str(control / "data" / definition.get("profile", "expert-profile.bin")),
            "--expert-cache",
            "auto",
            "--prefill",
            "auto",
            "--spec",
            "4",
            "--spec-min-p",
            "0.5",
            "--max-context",
            str(context),
        ]
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
        if backend == "metal":
            args = [
                "--pack",
                str(pack),
                "--native",
                str(native),
                "--native-dense-gguf",
                str(native),
                "--ple-gguf",
                args[args.index("--ple-gguf") + 1],
                "--expert-profile",
                str(control / "data" / definition.get("profile", "expert-profile.bin")),
                "--expert-cache",
                "64",
                "--prefill",
                "auto",
                "--spec",
                "2",
                "--suffix-draft",
                "0",
                "--max-context",
                str(context),
                "--mmap-experts",
            ]
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
    flags = {arg.split("=", 1)[0] for arg in extra}
    filtered = []
    index = 0
    while index < len(args):
        flag = args[index]
        end = index + 1
        if end < len(args) and not args[end].startswith("--"):
            end += 1
        if flag not in flags and not (flag == "--prefill" and "--chunk" in flags):
            filtered.extend(args[index:end])
        index = end
    cfg["args"] = filtered + extra
    Path(request["config"]).write_text(
        json.dumps(cfg, indent=2) + "\n", encoding="utf-8"
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--control", type=Path, required=True)
    parser.add_argument("--catalog", type=Path)
    parser.add_argument("--request", type=Path)
    options = parser.parse_args()
    setup = load_setup(options.control.resolve())
    if options.catalog:
        rows = catalog(setup)
        text = options.catalog.read_text(encoding="utf-8")
        options.catalog.write_text(
            update_catalog(text, rows), encoding="utf-8", newline="\n"
        )
    elif options.request:
        prepare(
            setup,
            options.control.resolve(),
            json.loads(options.request.read_text(encoding="utf-8")),
        )
    else:
        parser.error("--catalog or --request is required")


if __name__ == "__main__":
    main()
