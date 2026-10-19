"""Offline regression tests for the native Mermaid vendor inventory."""

import base64
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "tools/vendor_mermaid.py"
spec = importlib.util.spec_from_file_location("vendor_mermaid", SCRIPT)
vm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vm)

VERSION = "11.16.1"
URL = f"https://registry.npmjs.org/mermaid/-/mermaid-{VERSION}.tgz"
FILES = {
    "package.json": json.dumps(
        {"name": "mermaid", "version": VERSION, "license": "MIT"}
    ).encode(),
    "LICENSE": b"MIT License\nCopyright (c) Knut Sveidqvist\n",
    "README.md": b"Mermaid test fixture\n",
    "dist/mermaid.esm.mjs.map": b'{"version":3,"sources":["mermaid.ts"]}',
    "dist/mermaid.esm.mjs": b'import { sample } from "./chunks/mermaid.esm/core.mjs"; export default sample;\n',
    "dist/chunks/mermaid.esm/core.mjs": b'import "./flow.mjs"; export const sample = 123;\n',
    "dist/chunks/mermaid.esm/core.mjs.map": b'{"version":3,"sources":[]}',
    "dist/chunks/mermaid.esm/flow.mjs": b'export const diagram = "flowchart";\n',
}


def make_tarball(files):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w:gz") as tar:
        for name, body in (files.items() if isinstance(files, dict) else files):
            info = tarfile.TarInfo(f"package/{name}")
            info.size = len(body)
            tar.addfile(info, io.BytesIO(body))
    return stream.getvalue()


class VendorMermaidTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.vendor = self.root / "src/web-app/vendor/mermaid"
        self.lockfiles = [
            self.root / "src/app/package-lock.json",
            self.root / "src/web-app/package-lock.json",
        ]
        self.override = patch.multiple(
            vm, ROOT=self.root, DEST=self.vendor, LOCKFILES=tuple(self.lockfiles)
        )
        self.override.start()
        self.addCleanup(self.override.stop)
        self.archive = make_tarball(FILES)
        self.write_locks(self.archive)

    def write_locks(self, archive):
        integrity = (
            "sha512-" + base64.b64encode(hashlib.sha512(archive).digest()).decode()
        )
        for path in self.lockfiles:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(
                json.dumps(
                    {
                        "packages": {
                            "": {"dependencies": {"mermaid": VERSION}},
                            "node_modules/mermaid": {
                                "version": VERSION,
                                "resolved": URL,
                                "integrity": integrity,
                            },
                        }
                    }
                )
            )
            path.with_name("package.json").write_text(
                json.dumps({"dependencies": {"mermaid": VERSION}})
            )
        self.integrity = integrity

    def populate(self):
        # This name intentionally differs from the fixture's vendor directory.
        vm.prepare_tarball(
            self.archive,
            VERSION,
            URL,
            self.integrity,
            hashlib.sha512(self.archive).digest(),
        )

    def test_exact_offline_roundtrip(self):
        self.populate()
        vm.check_vendor(VERSION, URL, self.integrity)
        self.assertEqual(
            (self.vendor / "dist/chunks/mermaid.esm/core.mjs.map").read_bytes(),
            FILES["dist/chunks/mermaid.esm/core.mjs.map"],
        )

    def test_modified_and_extra_files_are_rejected(self):
        self.populate()
        target = self.vendor / "dist/mermaid.esm.mjs"
        target.write_bytes(b"tamper")
        with self.assertRaisesRegex(ValueError, "changed"):
            vm.check_vendor(VERSION, URL, self.integrity)
        target.write_bytes(FILES["dist/mermaid.esm.mjs"])
        (self.vendor / "extra.mjs").write_bytes(b"new")
        with self.assertRaisesRegex(ValueError, "Vendor file set differs"):
            vm.check_vendor(VERSION, URL, self.integrity)

    def test_broken_import_fails_and_preserves_previous_vendor(self):
        self.populate()
        before = (self.vendor / vm.MANIFEST).read_bytes()
        broken = dict(FILES)
        broken["dist/mermaid.esm.mjs"] = b'import "./chunks/mermaid.esm/missing.mjs";\n'
        archive = make_tarball(broken)
        with self.assertRaisesRegex(ValueError, "Broken ESM import"):
            vm.prepare_tarball(
                archive, VERSION, URL, self.integrity, hashlib.sha512(archive).digest()
            )
        self.assertEqual((self.vendor / vm.MANIFEST).read_bytes(), before)

    def test_archive_integrity_and_duplicate_entries(self):
        with self.assertRaisesRegex(ValueError, "SHA-512"):
            vm.prepare_tarball(self.archive, VERSION, URL, self.integrity, b"0" * 64)
        duplicate = make_tarball(list(FILES.items()) + [("LICENSE", FILES["LICENSE"])])
        with self.assertRaisesRegex(ValueError, "duplicated"):
            vm.prepare_tarball(
                duplicate,
                VERSION,
                URL,
                self.integrity,
                hashlib.sha512(duplicate).digest(),
            )

    def test_missing_upstream_license_fails_closed(self):
        missing = dict(FILES)
        del missing["LICENSE"]
        archive = make_tarball(missing)
        with self.assertRaisesRegex(ValueError, "license.*missing"):
            vm.prepare_tarball(
                archive, VERSION, URL, self.integrity, hashlib.sha512(archive).digest()
            )

    def test_version_before_security_fix_is_rejected(self):
        for lock in self.lockfiles:
            data = json.loads(lock.read_text())
            data["packages"]["node_modules/mermaid"]["version"] = "11.15.0"
            lock.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "security-fixed"):
            vm.locked_package()

    def test_tarball_directory_entry_is_allowed(self):
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w:gz") as archive:
            directory = tarfile.TarInfo("package/")
            directory.type = tarfile.DIRTYPE
            archive.addfile(directory)
            for name, body in FILES.items():
                item = tarfile.TarInfo("package/" + name)
                item.size = len(body)
                archive.addfile(item, io.BytesIO(body))
        body = stream.getvalue()
        vm.prepare_tarball(
            body, VERSION, URL, self.integrity, hashlib.sha512(body).digest()
        )
        vm.check_vendor(VERSION, URL, self.integrity)

    def test_path_traversal_rejected(self):
        files = dict(FILES)
        files["dist/chunks/mermaid.esm/../../unsafe.mjs"] = b"export {}"
        archive = make_tarball(files)
        with self.assertRaisesRegex(ValueError, "Unsafe"):
            vm.prepare_tarball(
                archive, VERSION, URL, self.integrity, hashlib.sha512(archive).digest()
            )

    def test_declared_version_must_be_exact(self):
        data = json.loads(self.lockfiles[0].read_text())
        data["packages"][""]["dependencies"]["mermaid"] = "^11.16.1"
        self.lockfiles[0].write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "Pin mermaid"):
            vm.locked_package()

    def test_mermaid_template_text_is_not_an_import(self):
        # Reproduces the failure from Mermaid 11.16.1 chunk-6QIBY7DQ.mjs.
        source = r"""const query = `MATCH (n) from '${source.modelEntityType}'`;
const example = `import Thing from 'not-an-esm-import';`;
const text = "from 'not-an-esm-import'";
// from 'also-not-a-dependency'
/* import 'still-not-a-dependency' */
const pattern = /import\s+\"imaginary\"/;
"""
        self.assertEqual(list(vm.esm_specifiers(source)), [])

    def test_nested_templates_preserve_real_interpolated_imports(self):
        source = 'const value = `outer ${ `inner ${import("./flow.mjs")}` }`;'
        self.assertEqual(list(vm.esm_specifiers(source)), ["./flow.mjs"])

    def test_literal_esm_specifiers_cover_all_import_forms(self):
        source = """import './setup.mjs';
import main, { helper } from './helper.mjs';
import * as lib from './lib.mjs';
export * from './reexports.mjs';
export { named } from './named.mjs';
const lazy = () => import('./lazy.mjs');
console.log(import.meta.url);
"""
        self.assertEqual(
            list(vm.esm_specifiers(source)),
            [
                "./setup.mjs",
                "./helper.mjs",
                "./lib.mjs",
                "./reexports.mjs",
                "./named.mjs",
                "./lazy.mjs",
            ],
        )

    def test_missing_dynamic_import_is_rejected(self):
        self.populate()
        main = self.vendor / vm.MAIN
        main.write_text(
            'export const broken = () => import("./chunks/mermaid.esm/lost.mjs");\n'
        )
        with self.assertRaisesRegex(ValueError, "Broken ESM import"):
            vm.verify_imports(self.vendor)

    def test_actual_external_import_is_rejected(self):
        self.populate()
        main = self.vendor / vm.MAIN
        main.write_text('import { a } from "unvendored-dependency";\n')
        with self.assertRaisesRegex(ValueError, "Non-relative ESM import"):
            vm.verify_imports(self.vendor)

    def test_reexport_missing_chunk_is_rejected(self):
        self.populate()
        main = self.vendor / vm.MAIN
        main.write_text('export { a } from "./chunks/mermaid.esm/lost.mjs";\n')
        with self.assertRaisesRegex(ValueError, "Broken ESM import"):
            vm.verify_imports(self.vendor)

    def test_git_tracking_required_for_ci(self):
        self.populate()
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        with self.assertRaisesRegex(ValueError, "not fully committed"):
            vm.check_vendor(VERSION, URL, self.integrity, tracked=True)
        subprocess.run(
            ["git", "-C", str(self.root), "add", "src/web-app/vendor/mermaid"],
            check=True,
        )
        vm.check_vendor(VERSION, URL, self.integrity, tracked=True)


if __name__ == "__main__":
    unittest.main()
