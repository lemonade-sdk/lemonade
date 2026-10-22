# Advanced offline Model Manager contracts

For complete installation, one-command execution, branch merging, and the
separate race-fix patch, see **[README.md](README.md)**.


Run **after** the original tests are green, against the PR #3828 + GUI3 merge build:

```bash
python3 test/model_compat/advanced_contract.py --lemond ./build/lemond --verbose
```

Run a targeted subset, for example:

```bash
python3 test/model_compat/advanced_contract.py --lemond ./build/lemond --match shared --verbose
```

The script imports `IsolatedLemond` from `server_contract.py`; it launches its
**own** `lemond` with private `HOME`, `models_dir`, config, and offline mode. It
cannot connect to an already-running Lemonade server. No GPU, real weights,
network, npm install, or inference backend is required. It does not modify any
application source, CI configuration, or test runner.

The tests cover:

- Incomplete and corrupted GGUF artifacts: corrupt headers, `.partial`,
  `.download_manifest.json`, and variant mismatch must not count as downloaded.
- True multimodal `main`/`mmproj`: separate repositories, legacy companion field,
  active snapshot revisions, and resolved file paths.
- Destructive operations: deleting one cached quantization must preserve the
  other; a shared `mmproj` must survive deletion of one of its owners; local
  external files and symlink targets must not be deleted.
- Registry provenance: same repo ID at Hugging Face and ModelScope must use
  separate cache namespaces and independent deletion.
- Snapshot consistency: simultaneous registrations/options writes, replacement
  of a model definition, and failed request atomicity.
- Persistence and integrity: alias cycle rejection, alias + options after
  daemon restart, valid catalogs beside broken catalogs, and extra-model name
  collisions across directories.

**Interpretation:** A failed test reports a behavior mismatch to investigate, not
necessarily a regression introduced by #3828. Compare against a main-branch
binary before attributing any failure to the refactoring. Only offline synthetic
GGUF headers are used; this suite cannot establish real download or inference
correctness. CI integration is intentionally separate.
