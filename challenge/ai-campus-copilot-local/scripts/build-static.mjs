/**
 * Builds the hosted static export.
 *
 * Two parts of the app cannot exist under `output: "export"`:
 *   - the Lemonade proxy route, which is deliberately dynamic;
 *   - /documents/[id], whose ids are minted at runtime and live only in the
 *     visitor's IndexedDB, so there is no set of paths to pre-render.
 *
 * Both are moved aside for the build and restored afterwards, including on
 * failure, so the working tree is never left missing a source file. The hosted
 * build reaches document details through /documents/view/?id= instead.
 */
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, renameSync, rmSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const projectRoot = join(dirname(fileURLToPath(import.meta.url)), "..");
const stashRoot = join(projectRoot, ".static-build-stash");

const EXCLUDED = [
  { live: join(projectRoot, "src", "app", "api"), stash: join(stashRoot, "api") },
  {
    live: join(projectRoot, "src", "app", "documents", "[id]"),
    stash: join(stashRoot, "documents-id"),
  },
];

function restore() {
  for (const { live, stash } of EXCLUDED) {
    if (!existsSync(stash)) continue;
    if (existsSync(live)) {
      console.error(`Refusing to restore: both ${live} and ${stash} exist. Merge them by hand.`);
      continue;
    }
    mkdirSync(dirname(live), { recursive: true });
    renameSync(stash, live);
  }
  if (existsSync(stashRoot)) rmSync(stashRoot, { recursive: true, force: true });
}

// Recover from a previous run that was killed before it could restore.
restore();

let exitCode = 1;
try {
  mkdirSync(stashRoot, { recursive: true });
  for (const { live, stash } of EXCLUDED) {
    if (existsSync(live)) renameSync(live, stash);
  }

  const result = spawnSync("npx", ["next", "build"], {
    cwd: projectRoot,
    stdio: "inherit",
    env: {
      ...process.env,
      BUILD_STATIC_EXPORT: "1",
      NEXT_PUBLIC_LEMONADE_DIRECT: "1",
    },
  });
  exitCode = result.status ?? 1;
} finally {
  restore();
}

process.exit(exitCode);
