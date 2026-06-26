"""Frozen-major schema lock — a shipped schema major must not change silently.

Back-compat hard rule: never edit or delete a released schema major; evolve only
by adding a new major (vN+1) schema file. This test makes that rule mechanical
(a snapshot guard, the cheap analog of `buf breaking`): it hashes a canonical
form of each versioned schema and compares it to ``schema-lock.json``.

Behavior on a hash mismatch:
  * ``released: true``  -> HARD FAILURE. A released major is immutable; ship a new
                          major schema file instead of editing this one.
  * ``released: false`` -> the schema is still under development; the edit is
                          allowed but you must refresh the lock in the SAME change:
                              python test/test_schema_lock.py --update
                          That turns every schema edit into a visible, reviewed
                          lockfile diff instead of silent drift.

Hashing a canonical (sorted-key, whitespace-stripped) JSON serialization means
the lock tracks *semantic* content only — reformatting or LF/CRLF differences do
not trip it, but any real structural change does.

Run:  python test/test_schema_lock.py            # check (also via unittest)
      python test/test_schema_lock.py --update   # refresh lock for unreleased majors
"""

import hashlib
import json
import os
import sys
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA_DIR = os.path.join(REPO_ROOT, "src", "cpp", "resources", "schemas")
LOCK_PATH = os.path.join(SCHEMA_DIR, "schema-lock.json")


def canonical_hash(schema_path):
    """sha256 of a canonical JSON serialization (sorted keys, no whitespace)."""
    with open(schema_path, "r", encoding="utf-8") as handle:
        obj = json.load(handle)
    canonical = json.dumps(obj, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def load_lock():
    with open(LOCK_PATH, "r", encoding="utf-8") as handle:
        return json.load(handle)


def update_lock():
    """Refresh hashes for every locked schema, preserving `released` flags.

    Refuses to silently re-lock a released major — that would defeat the guard.
    """
    lock = load_lock()
    changed = []
    for name, entry in lock.items():
        new_hash = canonical_hash(os.path.join(SCHEMA_DIR, name))
        if new_hash == entry["sha256"]:
            continue
        if entry.get("released"):
            raise SystemExit(
                f"refusing to re-lock released major '{name}': a released schema is "
                f"immutable — ship a new major schema file instead of editing it."
            )
        entry["sha256"] = new_hash
        changed.append(name)
    with open(LOCK_PATH, "w", encoding="utf-8") as handle:
        json.dump(lock, handle, indent=2)
        handle.write("\n")
    print("updated lock for:", ", ".join(changed) if changed else "(no changes)")


class SchemaLockTest(unittest.TestCase):
    def test_every_schema_is_locked(self):
        lock = load_lock()
        on_disk = {f for f in os.listdir(SCHEMA_DIR) if f.endswith(".schema.json")}
        self.assertEqual(
            on_disk,
            set(lock),
            msg="a *.schema.json file is not tracked in schema-lock.json (or vice "
            "versa); add it to the lock so it cannot change unnoticed",
        )

    def test_schemas_match_lock(self):
        lock = load_lock()
        for name, entry in lock.items():
            with self.subTest(schema=name):
                actual = canonical_hash(os.path.join(SCHEMA_DIR, name))
                if actual == entry["sha256"]:
                    continue
                if entry.get("released"):
                    self.fail(
                        f"released schema major '{name}' changed. A released major is "
                        f"immutable — do NOT edit it; ship a new major (vN+1) schema "
                        f"file. A changed lock for a released major is a breaking "
                        f"change and must be reviewed as one."
                    )
                self.fail(
                    f"schema '{name}' changed but its lock was not refreshed. If this "
                    f"is an intentional pre-release edit, run "
                    f"`python test/test_schema_lock.py --update` and commit the "
                    f"updated schema-lock.json in the same change."
                )


if __name__ == "__main__":
    if "--update" in sys.argv:
        update_lock()
    else:
        unittest.main(verbosity=2)
