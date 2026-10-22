# V1 compatibility baseline

Source: `integration/configured-backends` at
`410b5b4e9f9c7bdfcd1031ea6911795a8cce09ae`, Linux x86-64, default Release preset,
GCC 15.2.0 and system nlohmann JSON 3.12.0 from `/opt/rocm/core`.

`snapshot.json` was captured from a freshly built unchanged baseline `lemond` and
`lemonade`, using an isolated cache/config directory and ephemeral loopback port:

```sh
python3 test/backend_v1_snapshot.py capture --lemond build/lemond \
  --cli build/lemonade --out .synagent/artifacts/backend-v2-baseline
```

It retains the default and recipe-filtered schema exports, definition digests,
ETag and authentication responses, config defaults, generator-consumed recipe
metadata, CLI listing/schema, online/offline validation, exact warnings, and raw
schema/production-definition/defaults SHA-256 values. Fixture paths are excluded
from diagnostics by the existing public projection; no code or pointer is normalized.

`compiled.json` records the unchanged v1 validator's full inserted records,
recursive expression trees, and every operation-program field using SHA-256
fingerprints, plus the complete installed feature JSON and ordered diagnostics
for five independent invalid/external variations of every production definition.
It was captured by `test_backend_definition_v1_compat --capture` before changing
any v1 implementation, source, schema or descriptor path. `--capture` is an explicit
maintenance operation and is never invoked by CTest.

Compare the API/CLI capture with:

```sh
python3 test/backend_v1_snapshot.py compare --lemond build/lemond \
  --cli build/lemonade --baseline test/fixtures/backend-v1-compat
```

Both artifacts are comparison oracles. Do not regenerate them to accept drift.
The production v1 schema hash is
`43c4c93ac73f3cc88d9773c2ad55cdadc83f6497987c9ca8b449ceef4efff818`.
