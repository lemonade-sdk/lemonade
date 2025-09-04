import argparse
import importlib.metadata
import json
import shutil
import sys
from pathlib import Path
from tabulate import tabulate

try:  # optional import; lemonadesdk may not provide llamacpp utils
    from lemonade.tools.llamacpp.utils import get_llama_folder_path
except Exception:  # pylint: disable=broad-except
    get_llama_folder_path = None


def dist_size(dist):
    total = 0
    for f in dist.files or []:
        p = dist.locate_file(f)
        if p.is_file():
            total += p.stat().st_size
    return total


def dir_size(path: Path) -> int:
    total = 0
    for p in path.rglob("*"):
        if p.is_file():
            total += p.stat().st_size
    return total


parser = argparse.ArgumentParser()
parser.add_argument("--stats", help="Write JSON stats to this file")
parser.add_argument("--install-time", type=int, help="Installation time in seconds")
parser.add_argument("--zip-env", help="Path to environment directory to zip")
args = parser.parse_args()

rows = [(d.metadata["Name"], dist_size(d)) for d in importlib.metadata.distributions()]

if get_llama_folder_path:
    for backend in ("vulkan", "rocm"):
        backend_dir = Path(get_llama_folder_path(backend)).parent
        if backend_dir.is_dir():
            rows.append((backend, dir_size(backend_dir)))
else:
    base_dir = Path(sys.executable).resolve().parent
    for name in ("vulkan", "rocm"):
        extra = base_dir / name
        if extra.is_dir():
            rows.append((name, dir_size(extra)))

rows.sort(key=lambda x: x[1], reverse=True)

total = sum(size for _, size in rows)

print(f"Total installation size: {total/1024/1024:.2f} MB\n")
print("### Installation Size Breakdown\n")

table_data = []
for name, size in rows:
    mb = size / 1024 / 1024
    pct = (size / total * 100) if total else 0
    table_data.append((name, mb, pct))
table_data.append(("**Total**", total / 1024 / 1024, 100.0))

print(
    tabulate(
        table_data,
        headers=["Package", "Size (MB)", "% of Total"],
        tablefmt="github",
        floatfmt=".2f",
    )
)

zip_size = None
if args.zip_env:
    env_path = Path(args.zip_env)
    archive = shutil.make_archive(env_path.name, "zip", root_dir=env_path)
    zip_size = Path(archive).stat().st_size
    print(f"\nEnvironment zip size: {zip_size/1024/1024:.2f} MB")

if args.stats:
    data = {"size": total, "time": args.install_time}
    if zip_size is not None:
        data["zip"] = zip_size
    with open(args.stats, "w") as f:
        json.dump(data, f)
