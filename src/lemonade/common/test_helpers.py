import os
import shutil


def create_test_dir(
    key: str,
    base_dir: str = os.path.dirname(os.path.abspath(__file__)),
):
    # Define paths to be used
    cache_dir = os.path.join(base_dir, "generated", f"{key}_cache_dir")
    corpus_dir = os.path.join(base_dir, "generated", "test_corpus")

    # Delete folders if they exist and
    if os.path.isdir(cache_dir):
        shutil.rmtree(cache_dir)
    if os.path.isdir(corpus_dir):
        shutil.rmtree(corpus_dir)
    os.makedirs(corpus_dir, exist_ok=True)

    return cache_dir, corpus_dir


def strip_dot_py(test_script_file: str) -> str:
    return test_script_file.split(".")[0]


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
