# Decomposing `ModelManager` (and, by extension, the routing layer)

**Status:** proposal / for discussion. No code has been changed.
**Scope:** `src/cpp/server/model_manager.cpp`, its header, and the routing components it touches.
**Out of scope for this document:** UI changes, backend subprocess behavior, telemetry schema changes.

---

## 1. Purpose

`ModelManager` has grown into a single 6,731-line class with a 642-line header and 56 public
methods. It is the second-largest file in the server after `server.cpp` (8,302 lines), and it is
the hub that every backend and every API gateway transitively depends on: 39 translation units
include its header, and `server.cpp` alone calls 41 of its 56 public methods.

The goal of this work is twofold:

1. Split the class along its actual seams into components with one responsibility each.
2. Make **adding a new catalog source, artifact repository, or download source a local change** —
   one new file plus one registration line — rather than an edit to shared enums, factories, and
   base classes.

Large-scale refactoring and API breakage are both in scope.

### Non-goals

- **Not** rewriting `download_model` semantics while relocating it (see §7, Stage 6).
- **Not** splitting `ModelInfo`. It is the ubiquitous value object crossing 39 translation units;
  its `extras` map and `shared_ptr<const RoutePolicy>` already allow extension without editing it.
- **Not** creating one class per method. The target is roughly nine components plus four ports.
- **Not** dynamic plugin loading. Everything below assumes compile-time registration only.

---

## 2. Why it is one class today

### 2.1 The binding agent is mutable state, not logic

Mapping each mutable member to what touches it:

| State | Touched by | Notes |
|---|---|---|
| `server_models_`, `user_models_` | ingest + cache build | read by nearly everything |
| `models_cache_`, `public_model_aliases_`, `canonical_public_names_` | cache build + all reads | one hot mutex |
| `filtered_out_models_`, `recipes_all_models_filtered_` | `filter_models_by_backend` | **a `const` method mutating `mutable` side tables** |
| `sync_state_` (own mutex + condvar) | check / sync / enqueue / cancel | **disjoint from everything else** |
| `download_locks_`, `update_check_mutex_` | pull / update check | disjoint from cache state |
| `recipe_options_` + `recipe_options_write_mutex_` | options CRUD | own file, own mutex, no model-domain knowledge |

`sync_state_` and `models_cache_` never touch each other; they meet only through the registry.
That is the strongest available signal that the class can be split.

There are also 65 lock acquisitions across six distinct locks, and six test-only seams on the
public interface (`discover_extra_models_for_test`, `set_model_update_available_for_test`,
`set_update_check_override_for_test`, `set_download_model_override_for_test`,
`set_sync_phase_callback`, `recipes_all_models_filtered_snapshot`) — a sign that the seams exist
informally and tests reach around the design rather than through it.

### 2.2 The structure is already there, it just has no boundaries

Two parts of the code are already decomposed and simply not expressed as such:

- **`build_cache()` is a numbered pipeline.** Its own comments label the steps: Step 1 (L3125,
  built-in + user ingest), Step 1.5 (L3281, extra-models-dir scan), Step 1.6 (L3304, backend
  dynamic discovery), Step 2 (L3382, hardware filter), Step 3 (download status + metadata), and
  an alias rebuild at the end.
- **The download path is already two-phase.** `download_from_registry` (L5819) builds a JSON
  manifest and hands it to `download_from_manifest` (L5500, 318 lines). The second function's
  entire input contract is `{download_path, files_count, files[{name, url, size,
  hash{algorithm,value}}]}` plus headers, and it contains **no registry, provider, snapshot, or
  revision reference at all** — it does retry, resume, hash verification, progress, and cancellation
  for anything that can produce that plan.

### 2.3 Five defects this is currently causing

Each has a target section in §3-§6 where it stops being possible.

**(a) An existing abstraction is bypassed.** `ModelRegistry` (`model_registry.h:126`) is a clean
provider interface — `source()`, `default_revision()`, `auth_headers()`, `fetch_repository()`,
`resolve_file_url()`. The update-check path bypasses it entirely: `fetch_hf_file_metadata_for_ref`
(L5376) re-implements provider access against `$HF_ENDPOINT/api/models/<repo>/tree/<ref>` and
re-parses `lfs.oid` / `oid` into a hash, duplicating the `RegistryFile` normalization already done
in `model_registry.cpp:359`. Two providers, two parallel fetch paths, guaranteed drift. Its helper
`hf_file_metadata_from_tree_file` (L5348) is a second copy of the same field mapping. → §4.1

**(b) A closed enum blocks extension.** `RemoteRegistrySource` (`model_registry.h:15`) is a hard
enum with two values. Adding a source means editing: the enum in a widely-included header, the
if/else factory (`model_registry.cpp:1039`), `parse_remote_registry_source`,
`remote_registry_display_name`, and the `models--` / `modelscope--` ternary in
`registry_repo_cache_dir_name` (`model_registry.cpp:1028-1037`). → §4.1, §5.1

**(c) Capability checks are done as type identity.** Two sites read
`if (source == RemoteRegistrySource::HuggingFace)` (L2070, L5936) to mean "this source exposes
content ids that survive revisions and can pin an active revision." A third source with the same
property must be special-cased rather than declared. → §4.1

**(d) A dependency inversion.** `BackendOps` is the per-backend policy interface and is the one
place in this area that already gets the pattern right — every method has a default, so adding a
method never forces edits on backends that do not override it. But its defaults call *up* into
`ModelManager` (`backends/backend_ops.cpp:114-126`):

```cpp
bool BackendOps::is_downloaded(...) const {
    return ctx.model_manager != nullptr && ctx.model_manager->checkpoints_complete(info);
}
void BackendOps::download_model(...) const {
    if (ctx.model_manager != nullptr) {
        ctx.model_manager->download_from_registry_engine(info, progress);  // model_manager.cpp:2973
    }
}
```

The backend-policy base class depends on the god class. → §4.5

**(e) There is no artifact-source abstraction.** "Where do the bytes come from?" is answered by one
string field plus scattered equality checks — and the same field also records which catalog
registered the model, so both axes from §2.4 live in a single string:

| Site | Behavior |
|---|---|
| `ModelInfo::source` (header L102) | one string, values `local_upload` / `local_path` / `extra_models_dir` / `huggingface` / `modelscope` — catalog origin **and** byte source conflated |
| `resolve_model_path` (L1590-1624) | three-way switch: `local_path` returns the checkpoint **as-is**; `local_upload` roots it **inside** the HF cache dir; otherwise registry cache layout + `ops_for(recipe)->resolve_checkpoint_path()`; collections return `""` |
| `server.cpp:2954-2959` | validates `source` against a mixed list in one if/else: `is_remote_registry_source(...)` **or** the three local tokens |
| `type == "npu_cache"` skipped | **five copies**: L2951, L5188, L5860, L6237, L6292 |
| `WhisperServer::download_npu_compiled_cache` (`whispercpp_server.cpp:155`) | a whole second artifact pipeline inside a backend: its own repo fetch, its own path-traversal guards, its own placement rule |
| `checkpoint_looks_like_repo_id` (`model_registry.cpp:864`) | heuristic separating `owner/repo` from self-managed tags such as `gemma3:4b` |
| `backend_self_manages_downloads` (L4345) | `flm` pulls its own weights |
| cloud | `downloaded = true`, `size = 0` (`cloud_server.cpp:1082-1083`) — no bytes exist at all |
| `delete_model` (L6091-6240) | FLM → `flm remove`; shared repo → variant only; otherwise walk up for a `models--*` ancestor and **throw** if absent (L6196) |

The repeated `npu_cache` exclusion is the symptom: a checkpoint whose bytes come from a different
place, with different placement rules, has no way to be expressed — so it is skipped in five
functions and special-cased in one backend. → §4.6

### 2.4 Why it resists decomposition: two axes are interleaved

The reason this class is hard to split is that it interleaves two orthogonal concepts, often within
a single function:

- **Catalogs** answer *which models exist?* `server_models.json`, `user_models.json`, a directory
  scan, a cloud provider's model listing, a collection manifest.
- **Artifact repositories** answer *where do the bytes live, and are they current?*
  Hugging Face, ModelScope.

`download_from_registry` interleaves catalog-driven file selection with repository protocol with
HTTP transfer. `check_for_model_updates` interleaves catalog iteration with repository staleness.
Any decomposition that picks only one of the two axes ends up with a component that still has to
reach across. §3 starts by separating the axes, and the components fall out of that.

One wrinkle decides the shape of the artifact axis. Not every model's bytes come from a place that
can be *listed* or *polled*: an imported directory, an `extra_models_dir` scan, and a cloud model
have no revision, no file tree, and nothing to fetch. So the artifact axis splits internally into
two roles — **resolve and validate what is here** versus **list, compare, and pull** — and only
some sources implement the second. The two-axis picture above still holds; §4.6 does the internal
split, and it is what makes defect (e) expressible.

---

## 3. Target model: two axes, joined by provenance

```
AXIS 1: which models exist?              AXIS 2: where are the bytes?
-------------------------------          ---------------------------------------------
ModelCatalog  (many instances)           ArtifactSource          (one PER CHECKPOINT)
  JsonCatalog(Locator)                     resolve / present / acquisition /
    Locator = File | Resource | Http       deletion / provenance
  ScanCatalog(Directory)                 FetchableRepository : ArtifactSource
  ListedCatalog(provider)                  list / file_url / active_revision /
  +N                                       compare / commit / transfer / layout
        |                                          ^          ^
        |          ArtifactRef                     |          |
        +---------> ArtifactSourceResolver --------+----------+
                     (the ONLY site that picks a source)
                     HF · ModelScope · Filesystem
                     Local · SelfManaged · None
                              |
                     TransferEngine + ArtifactLayout
                     (shared mechanism: HTTP | local copy)

Both axes feed ModelStore -> StoreSnapshot (immutable, atomic swap, lock-free reads)
ModelStore -> SourceChangeBus -> ModelSyncService;  ModelIngest orchestrates across both axes
ModelArtifactStore = local disk mechanics (snapshots, blobs, .partial, provenance files)

  Consumers: server.cpp, ollama_api, mcp_server, anthropic_api
             -> ModelStore | ModelIngest | RecipeOptionsStore | RoutingService
```

### 3.1 `ModelCatalog` — an instantiable interface, not a singleton

**Definition: a catalog is anything that can (1) enumerate model entries and (2) report when its
enumeration changed.** The second half is what makes change notification and auto-update
generalizable instead of HF-specific.

The generalization is nearly free in the current code — the shape already exists three times:

| Current code | Locator | Failure policy |
|---|---|---|
| `load_server_models()` (L1632) | `get_resource_path("resources/server_models.json")` | **throws** — "critical file required for the application to run" |
| `user_models_` ← `load_optional_json(get_user_models_file())` (L1055, L1645) | config-dir path | degrades to `{}` |
| `load_architecture_defaults()` (L1659) | resource path | warns, degrades |

One shared parse primitive, three locators, three error policies. So:

```cpp
class ModelCatalog {
public:
    virtual CatalogId id() const = 0;
    virtual bool required() const { return false; }   // fail-fast vs degrade

    // Add or override entries during a store build.
    virtual void enumerate(CatalogAccumulator&) const = 0;

    // Did the ENUMERATION change? (mtime, ETag, listing digest)
    virtual bool enumeration_changed(const CatalogState&) const = 0;
};

JsonCatalog(FileLocator{"user_models.json"},     /*required=*/true)
JsonCatalog(ResourceLocator{"resources/server_models.json"}, /*required=*/true)
JsonCatalog(HttpLocator{url, etag, cache_path})       // remote catalogs by URL
ScanCatalog(DirectoryLocator{extra_models_dir})       // staleness = watcher event
ListedCatalog(provider, base_url)                     // enumerate = discover_models
```

All five of today's sources satisfy the definition, including the two that are not documents: the
directory scan (staleness = an inotify/kqueue event) and cloud listings (staleness = listing
digest). This also **removes a concept** — an earlier draft of this document proposed a separate
`ModelSourceContributor` port, which turns out to be the same interface. A new source is one
`ModelCatalog` implementation.

Two precision points:

- **The catalog test is "do the keys name models?"** `architecture_defaults.json` and
  `recipe_options.json` are JSON documents in the same directory but are *not* catalogs — they are
  option overlays keyed by architecture and by canonical ID respectively. Keep them out of this
  abstraction (the latter gets its own component, §3.2).
- **Locator is a second axis inside the catalog.** `HttpLocator` is where a remote URL catalog
  actually lands, and it can express ETag / Last-Modified / on-disk cache semantics that
  `load_optional_json` structurally cannot. Per-catalog lazy refresh also lets a URL catalog's
  network fetch happen off the startup path, while `server_models.json` keeps its fail-fast
  behavior via `required()`. Today the constructor eager-loads all four documents
  (`model_manager.cpp:1002-1005`).

### 3.2 `ModelStore` — the aggregate (renamed)

The merged result is **not** a catalog; it is a view over N of them. It owns merge, precedence
(`user.` > `extra.` > `builtin.`), public aliasing, hardware filtering, and download status, and it
publishes an immutable `StoreSnapshot`.

Keeping the aggregate under a different name from the per-document interface is what makes the
`user.` / `extra.` / `builtin.` stamping work: those are **provenance assigned at merge time**, not
catalog types. That is what preserves the closed-grammar guarantee in §5.1 while catalogs stay
open.

### 3.3 Component map

| Target component | Extracted from `model_manager.cpp` | Notes |
|---|---|---|
| **`ModelStore`** + `StoreSnapshot` | `models_cache_`, `public_model_aliases_`, `canonical_public_names_`, `get_supported_models` (L1881), `get_downloaded_models` (L3694), `resolve_model_name` (L6367), `get_public_model_name` (L6375), `model_exists*`, `get_model_info*` (L6349-6731) | Immutable snapshot, atomically swapped, lock-free reads |
| **`StoreBuilder`** | `build_cache` (L3112, 392 lines) | Pipeline of stages following the steps already numbered in the source |
| **`HardwareFilterStage`** | `filter_models_by_backend` (L3787, 230 lines), `recipes_missing_all_models`, `streaming_working_set_gb`, `streaming_model_exceeds_pool` | Returns `{visible, rejected[{model, reason}], empty_recipes}` **as a value**. Removes the `track_recipe_availability` footgun documented in the header |
| **Catalogs** (`JsonCatalog` ×3, `ScanCatalog`, `ListedCatalog`) | `load_server_models` (L1632), `load_optional_json` (L1645), `discover_extra_models` (L1258, 190 lines), `discover_extra_models_in_directory` (L1449), `identify_gguf_models` (L818, 181 lines), `refresh_cloud_models` (L4048), `evict_cloud_models` (L4141), `count_cloud_models` (L4162), `set_cloud_registry` (L4044), `set_extra_models_dir` (L1118), `start_directory_watcher` (L1140) | Each is one `ModelCatalog`. The directory watcher becomes catalog staleness rather than a cache-invalidation hack |
| **`ModelIngest`** (use case) | `download_model` (L4734, **613 lines**), `register_model` (L4712), `register_user_model` (L4210), `unregister_user_model` (L4294), `validate_collection_request` (L6400, 158 lines) | Registration, validation, rollback, cycle guard. Delegates transfer |
| **`CollectionResolver`** | `fetch_collection_manifest` (L4502), `register_components` (L4557), `resolve_collection_components_from_manifest` (L4661), `populate_collection_components_from_cache_locked` (L4668) | A collection manifest is a nested catalog; this is its adapter |
| **`ModelSyncService`** | `ModelSyncState`, `check_for_model_updates` (L1912, 387 lines) → `scan_for_updates()`, `execute_sync` (L2433, 279 lines), `sync_models` (L2713), `enqueue_sync` (L2374), `get_sync_status` (L2348), `cancel_sync` (L2368), `should_auto_update` (L2300) | ~600 lines, own threading. Already state-disjoint from the store |
| **`ModelArtifactStore`** | `resolve_all_model_paths` (L1626), `checkpoints_complete` (L2969), `list_model_files` (L627), `delete_model` (L6091, 183 lines, file half), `cleanup_orphaned_cache` (L6275), blob/partial/provenance helpers (L465-818) | Local disk mechanics only. `resolve_model_path`'s three-way `source` switch moves to `LocalPathArtifactSource` (§4.6); the store stops implying that snapshot layout is universal |
| **`RecipeOptionsStore`** | `get_user_models_file` / `get_recipe_options_file` (L1055-1060), `save_user_json` (L1680), options CRUD (L1735-1881), ctor migration block (L1006-1050) | Own file, own mutex, three-level merge. **Extract first** |
| **`SourceChangeBus`** | `notify_models_changed` (L1081), `next_notify_generation` (L1077), `invalidate_models_cache` (L1067) | Carries `{catalog_id \| repository_id, generation}`; keeps the "fire outside all locks" rule |

### 3.4 Concurrency change

Today: one hot `models_cache_mutex_` on the read path, and a `const` filter method that mutates
`mutable` side tables, with a header comment warning that `track_recipe_availability` may only be
true on a full-store build.

Target: the store is an immutable snapshot built off-thread and published with an atomic `shared_ptr`
swap. Readers take no lock. Filter rejections become fields of the snapshot. The "only true on full
builds" constraint stops being something people must remember and becomes structurally impossible
to violate.

This is the same idiom `RoutingPolicyStore` already uses (`routing_policy_store.h`: build a complete
immutable snapshot, atomically swap, readers see the old valid set or the new valid set) and the
same generation numbering the change bus provides. There should be **one** such idiom in the
server, not three.

### 3.5 Glossary

| Name | Meaning |
|---|---|
| `ModelCatalog` | Anything that enumerates model entries and reports enumeration changes. Many instances |
| `Locator` | How a catalog is read: file, embedded resource, HTTP, directory, provider listing |
| `ModelStore` / `StoreSnapshot` | The merged, filtered, precedence-resolved view over all catalogs |
| Repository | Shorthand for a `FetchableRepository`: a byte-hosting source that can be listed, polled, and pulled — Hugging Face, ModelScope, a file share |
| `ArtifactRef` | The provenance value joining a catalog entry to a repository: registry id, repo id, snapshot, variant, files |
| `ArtifactLayout` | On-disk shape of a repository's cache. Default = HF-hub-compatible snapshot/blob layout |
| `TransferEngine` | Mechanism that executes a `DownloadPlan`. Default = the existing HTTP downloader |
| `SourceChangeBus` | Generation-numbered notifications keyed by catalog or repository id |
| `ArtifactSource` | Per-checkpoint role: where the bytes are, whether they are present, how to acquire them, how to delete them |
| `FetchableRepository` | Optional role layered on an `ArtifactSource`: can be listed, polled for staleness, and pulled |
| `ArtifactSourceResolver` | The only place that maps a checkpoint to an `ArtifactSource`; replaces the `info.source` switch |

---

## 4. Extension ports

The pattern to copy is `BackendOps`: virtuals with defaults, never edited when a new implementation
appears. Everything assumes compile-time self-registration — no `dlopen`, no C ABI, no plugin
boundary. A registry of factories keyed by a string id, populated by static initializers, is
sufficient.

### 4.1 `FetchableRepository` — one source owns all its *policy* primitives

This is the **pullable** role. It is layered on the core `ArtifactSource` role defined in §4.6; not
every artifact source can be pulled, and the ones that can should own every decision that differs
per source. The naming is deliberate: "repository" matches the domain vocabulary (`repo_id`) and
signals namespace ownership.

```cpp
// The pullable role. Inherits everything in ArtifactSource (§4.6).
// Implementations: HuggingFaceRepository, ModelScopeRepository, FilesystemRepository.
class FetchableRepository : public ArtifactSource {
public:
    virtual RegistryId id() const = 0;
    virtual std::string display_name() const = 0;
    virtual std::string default_revision() const = 0;      // "main" vs "master"
    virtual RepositoryCapabilities capabilities() const { return {}; }

    // Metadata
    virtual RepoListing list(const std::string& repo_id,
                             const std::string& revision = "") const = 0;
    virtual std::string file_url(const RepoRef&) const = 0;
    virtual std::map<std::string, std::string> auth_headers() const { return {}; }

    // Byte identity and staleness. This group is what actually differs per
    // source, and it is what kills the `source == HuggingFace` special-cases.
    virtual bool can_go_stale() const = 0;                       // see §4.6
    virtual Revision    active_revision(const LocalRef&) const = 0;      // refs/main
    virtual void        set_active_revision(const LocalRef&, const Revision&) = 0;
    virtual Staleness   compare(const LocalRef&, const RepoListing&) const = 0;
    virtual void        commit(const LocalRef&, const Revision&) = 0;    // after transfer

    // Local namespace
    virtual std::string cache_dir_name(const std::string& repo_id) const = 0;

    // Mechanism selection, not mechanism duplication
    virtual TransferEngine& transfer() const { return default_http_transfer(); }
    virtual const ArtifactLayout& layout() const { return default_hf_layout(); }
};

LEMONADE_REGISTER_ARTIFACT_REPOSITORY(HuggingFaceRepository);   // one line, one .cpp
LEMONADE_REGISTER_ARTIFACT_REPOSITORY(ModelScopeRepository);
```

**Policy vs mechanism — the line that must hold.** Left column is per-source; right column is
shared, because duplicating it per source is how a refactor of this size re-creates the god object.

| Repository owns (today stranded as HF-specific statics in `model_manager.cpp`) | Shared mechanism |
|---|---|
| `auth_headers()`, endpoint/env resolution (`HF_ENDPOINT`, `MODELSCOPE_*_TOKEN`) | transfer: retry, resume, hash verify, progress, cancel |
| `list()`, `file_url()` | blob/snapshot layout + journaling (`.partial`, `.download_manifest.json`) — behind `ArtifactLayout`, default = today's |
| revision semantics: `read_hf_ref_main` (L268), `write_hf_ref_main` (L336), `active_hf_snapshot_path` (L326) — HF only; ModelScope's branch API has no commit pin | the sync queue, cancellation, progress reporting |
| staleness policy: HF compares LFS content ids across commits; ModelScope uses tree fingerprints (`model_registry.cpp:516`, `can_reuse_previous_hf_snapshot` L5308) — today expressed as `source == RemoteRegistrySource::HuggingFace` at L2070 and L5936 | catalog merge, precedence, public aliasing |
| cache directory naming: the `models--` / `modelscope--` ternary (`model_registry.cpp:1028-1037`) | notification bus |
| artifact selection hooks (`select_checkpoint_files`) | |

**There is deliberately no `Repo::download()`.** `download_from_manifest` (L5500, 318 lines) has
zero registry knowledge (§2.2) — proof that transfer is mechanism, not per-source policy. Giving
each repository a download method means N copies of the retry/resume/hash/cancel loop, and it
recreates the inversion in §2.3(d) where a source object reaches sideways into orchestration.
The orchestrator instead becomes roughly fifteen readable lines:

```
list -> select files -> build DownloadPlan -> repo.transfer().execute(plan) -> repo.commit()
```

If a future source needs a genuinely different transfer protocol (git-LFS batch API, S3 signed
multipart), that is a `TransferEngine` selected by the repository — visible in the sketch above —
and still not a per-repository copy of the loop.

Adding a repository becomes: one new `.cpp` plus the registration line. No enum, no factory branch,
no display-name switch, no cache-directory ternary, no new `if (source == ...)` anywhere. Because
every capability and optional method has a default, adding a *capability* later never edits
existing repositories either.

**Migration note:** `parse_remote_registry_source` must keep accepting the strings currently
persisted in `user_models.json` (`registry_source`) and `.lemonade_registry.json` (`source`).
Those on-disk strings are the real interface; the enum was incidental. Keep the string form
(`RegistryId`) as canonical identity.

### 4.2 `ModelCatalog` + `Locator`

Specified in §3.1. Registration is a store-composition concern, not a global registry: `Server`
constructs the catalog set (builtin resource, user file, extra dir, configured cloud providers, and
later any configured URL catalogs) and hands it to `ModelStore`. That keeps catalog *set* policy
visible in one place while catalog *behavior* stays open.

### 4.3 `TransferEngine`, `ArtifactTransfer`, `DownloadPlan`

The manifest JSON that `download_from_registry` builds becomes a typed `DownloadPlan` preserving
today's exact contract: `download_path`, per-file `{name, url, size, expected_hash}`, plus totals.
`ArtifactTransfer::execute(plan, progress_sink, cancel_token)` keeps the existing behavior
verbatim: 10 retries with exponential backoff (2 s → 120 s), `.partial` resume, low-speed
detection, expected-hash verification, GGUF magic-number rejection before a cached file is trusted
(L5622-5634), and cancellation propagation.

### 4.4 `ArtifactLayout`

The snapshot / `refs/main` / blob / `.partial` / `.lemonade_registry.json` layout is currently
assumed global but is really "Hugging Face hub compatible." Naming it as a port lets a future
repository choose a flat layout without fighting `ModelArtifactStore`. The default implementation
is today's behavior, so nothing else changes.

### 4.5 Fixing the inversion

Resolves §2.3(d). `BackendOps` gets the pattern right everywhere else — every method has a default,
so adding a method never forces edits on backends that do not override it — but its defaults call
up into `ModelManager`. It should instead return a **`DownloadDirective`** — "resolve via repository
X with variant selection Y", "self-managed, the backend pulls its own weights", "no artifacts" —
and the ingest use case performs the download. That removes the last upward dependency from backend
policy to the model layer, and it is where `backend_self_manages_downloads` (L4345) naturally
lands.

### 4.6 Artifact sources: the role split

§4.1 specifies the **pullable** role. `list()`, `file_url()`, `active_revision()`, and `compare()`
are meaningless for bytes that are already on disk, so they cannot be the interface every model is
asked through. Resolving defect (e) in §2.3 means splitting the artifact axis by **role**, not only
by implementation. One interface for every source would force a local directory to implement
`active_revision()` — the same wrong-abstract-base mistake this document criticizes elsewhere.

```cpp
// Every model that has bytes has exactly one of these PER CHECKPOINT.
class ArtifactSource {
public:
    virtual ArtifactKind kind() const = 0;   // registry | filesystem | local | self-managed | none

    virtual std::string resolve(const CheckpointRef&) const = 0;  // where the bytes are now
    virtual Presence    present(const ModelInfo&) const = 0;      // exists + backend validates
    virtual Acquisition acquisition(const ModelInfo&) const = 0;  // Nothing | Plan | Delegated | NotApplicable
    virtual Deletion    deletion(const ModelInfo&) const = 0;     // Nothing | CacheDir{shared} | Delegate(reason)
    virtual Provenance  provenance(const ModelInfo&) const = 0;
};

// The pullable role, repeated here only to express the relationship; the full
// member list is in §4.1. One member is worth calling out on its own:
class FetchableRepository : public ArtifactSource {
public:
    // The update scanner asks this instead of testing is_remote_registry_source(),
    // which is how "can this be stale at all?" is decided today.
    virtual bool can_go_stale() const = 0;
};
```

| Implementation | Roles | list / file_url | staleness | layout | transfer | deletion |
|---|---|---|---|---|---|---|
| `HuggingFaceRepository` | both | tree API | commit SHA + LFS content id | HF snapshots + blobs + `refs/main` | HTTP | cache dir; GC only unreferenced blobs (`is_repo_shared`, L372) |
| `ModelScopeRepository` | both | files API | tree fingerprint | snapshot per tree | HTTP | same shape, own namespace |
| **`FilesystemRepository`** | both | **walk a tree** | **size + mtime + optional checksum sidecar** | flat, no snapshots | **local copy** | directory; no blob GC |
| `LocalPathArtifactSource` | core only | n/a | never stale — bytes are already here | n/a | n/a | **never delete user-owned bytes**; unregister only |
| `SelfManagedArtifactSource` | core only | n/a | the backend's problem | the backend's | `Delegated` | `Delegate("flm remove")` |
| `NoArtifactsSource` (cloud) | core only | n/a | never | n/a | `NotApplicable` | unregister only |

`FilesystemRepository` is a real implementation, not a stub: it makes a LAN share, a mounted
volume, or an unpacked offline bundle into a pullable source, which is the air-gapped and
corporate-mirror case. It composes with both ports already specified — `TransferEngine` (§4.3) gains
a second implementation that copies and verifies instead of downloading and resuming, and
`ArtifactLayout` (§4.4) gains a flat variant with no snapshots or symlinks. No change to `ModelStore`
or `ModelSyncService` is required to accommodate it, and that is the test of whether the ports are
right.

**Selection lives in exactly one place**, applied per checkpoint over `info.checkpoints`, replacing
the seven scattered sites listed in §2.3(e):

```
for each checkpoint in info.checkpoints:
  1  recipe == "cloud"                       -> NoArtifactsSource
  2  BackendOps::plan_download() says self-   -> SelfManagedSource
     managed
  3  entry source is local_path / local_upload/ -> LocalPathArtifactSource
     extra_models_dir
  4  entry names an explicit scheme           -> that source (hf://, ms://, file://)
  5  checkpoint_looks_like_repo_id()          -> registry from entry.registry_source,
                                                 else default_pull_source
  6  path exists on disk                      -> LocalPathArtifactSource
  7  otherwise                                -> registration error
```

This stays open at both ends instead of becoming a new god switch: step 4 dispatches through the
repository registry (§4.1) and step 2 through the `DownloadDirective` (§4.5). A new registry adds a
repository; a new self-managed backend overrides `plan_download()`; a new local scheme adds one case
to this list — which is the one list legitimately allowed to grow, because it *is* the product's
source model.

**Selection is per checkpoint, not per model.** The current code says otherwise — the comment at
L5856 declares "a registration has one provenance and update domain" — but that is a policy choice,
not a fact about the world, and `npu_cache` already contradicts it: whisper fetches a different
repository, places the file by a different rule ("must sit next to the .bin"), and applies its own
traversal guards, while five functions exclude that checkpoint type. `ModelInfo::checkpoints` is
already a map and `registry_files::DeterminationTracker` already handles models spanning several
repositories, so widening selection to per-checkpoint is cheap now and expensive later. Model-level
status (`downloaded`, `update_available`) becomes an aggregate over checkpoint-level answers. See
§9 Q6 for the validation rule this touches.

**Destructive semantics must be polymorphic.** Local → never delete the bytes. Registry → delete the
cache dir but GC only unreferenced blobs. FLM → delegate. Cloud → nothing. Today that is four paths
through a 183-line function plus one string comparison in the right place, and a forgotten branch
deletes a user's model directory. One rough edge observed while reading: `delete_model` has no
local-source case, so for a `local_path` model `resolved_path()` is non-empty, the `models--`
ancestors walk throws (L6196), and deletion fails *safe* but does not complete. Intended behavior
for imported models is recorded as §9 Q7.

**Why catalogs and artifact sources look mergeable, and must stay separate.** For local artifacts
they are the same object: `ScanCatalog` enumerated the file, so it already knows where the bytes
are, and one class can implement both `ModelCatalog` and `LocalPathArtifactSource`. A `JsonCatalog`
entry pointing at Qwen on Hugging Face resolves to two entirely different objects. So: a catalog
entry produces an `ArtifactRef`, and the resolver turns an `ArtifactRef` into an `ArtifactSource`.
Sometimes one object answers both questions; the types stay separate because usually they do not.
That is also why `ArtifactRef` is the right join between the axes — it is the value a catalog emits
and a source consumes.

---

## 5. Identity: handles, provenance, and two kinds of staleness

Two distinct identity axes are currently conflated:

| Axis | Content | Extensible? | Used by |
|---|---|---|---|
| **Handle** | `<source-token>.<bare>` | **No — deliberately closed** | user input, `user_models.json` keys, `recipe_options.json` keys, API `model` field, `Decision::route_to` |
| **Provenance** (`ArtifactRef`) | `{registry_id, repo_id, snapshot_id, variant, files}` | **Yes — this is the join** | cache directory naming, blob dedup, update check, `.lemonade_registry.json` |

Today provenance is scattered across `registry_source`, `checkpoints[]`, `cloud_provider`, and the
cache-directory naming rule. Collecting it into one value is the composition that matters: the
artifact key becomes `f(registry, repo, snapshot)` without touching the ID grammar. Blob-level
sharing across models already works on exactly this basis.

### 5.1 The two enums: open one, keep one closed

**`RemoteRegistrySource` must open.** Replace with an interned `RegistryId` (string) plus the
repository registry (§4.1). This is the blocker for adding repositories.

**`ModelSource` / the canonical prefixes `user.` `extra.` `builtin.` must stay closed.** It is not
an extension point, it is a security boundary: `is_reserved_registration_name` prevents
`user.builtin.Foo` from hijacking a built-in alias slot, and `precedence_rank` defines shadowing
(`canonical_id.h`). New catalogs register into the store; they do not mint new source tokens.

A middle segment (`user.<registry>.<name>`) was considered and rejected for three reasons:

1. **Parser meaning would depend on which sources are compiled in.** Bare names legally contain
   `.` (for example `Qwen2.5-7B-Instruct`), so `user.hf.qwen` is only decidable by longest-match
   against the known id set. Compile-time registration makes that possible, but it means adding a
   source in a future release can silently re-parse IDs already persisted in `user_models.json` and
   `recipe_options.json`. Keys used on disk must never change meaning between builds.
2. **The hijack space becomes combinatorial.** `user.<token>.X` would be reserved only if `<token>`
   is not a *future* source id — a check that can pass today and fail after an upgrade, on data
   already on disk.
3. **It re-exposes what the precedence rule hides.** The "emit the bare name unless shadowed"
   design exists to keep provenance out of the UI.

### 5.2 Minting rule

```
handle ::= <source-token> '.' <bare>     // closed set: user | extra | builtin
bare   ::= [ <locality> '.' ] <name>     // locality ONLY when the ID is machine-minted
```

| Source | Authored by | Locality | Notes |
|---|---|---|---|
| `builtin.` | `server_models.json` | none | registry stays an attribute |
| `user.` | **human** | **never** | registry stays in the `registry_source` field |
| `extra.` | directory scan | registry of origin when inferable | today: unprefixed |
| cloud | provider listing | provider | **already implemented** |

The precedent is already in production: `CloudServer::build_public_name`
(`cloud_server.cpp:250-275`) returns `provider + "." + cleaned`, consumed at `cloud_server.cpp:1075`.
Cloud models ship today as `fireworks.deepseek-ai/deepseek-v3`. The one source whose IDs are
machine-minted already composes locality into the name, because it is the one place where a
collision is structurally unavoidable. The rule generalizes that.

A new catalog registers a **locality token**, which is an open set; `ModelSource` stays closed, so
`precedence_rank` and the reservation guard keep working unchanged.

**Gap this exposes:** `is_reserved_registration_name` currently knows only `builtin.` and `extra.`.
Cloud entries live in the bare-name space (`fireworks.deepseek-v3` carries no source token), so a
user can register `user.fireworks.foo` and shadow a cloud entry's public name via precedence.
Today that is one provider-collision away from being confusing, and remote URL catalogs multiply
the risk. Since the locality token set is known at compile time, the guard becomes a cheap static
check: extend it to reject `user.<known-locality>.…`. Add a unit test asserting that every
catalog's minted IDs round-trip through `parse_canonical_id` and survive
`is_reserved_registration_name`, making the minting convention a checked contract rather than a
comment.

**Accepted limitation:** `user.` cannot hold two same-named repositories from different registries.
A registry change is a provenance change, which should be an explicit replace with a conflict
error, not two coexisting IDs the user has to disambiguate. The existing defensive comments about
never silently replacing a user model's remote checkpoint (`model_manager.cpp:4832-4834`) already
point this way.

### 5.3 Two kinds of staleness (currently one flag)

Separating the axes exposes a modeling gap that matters for remote URL catalogs:

| | **Catalog staleness** | **Artifact staleness** |
|---|---|---|
| Question | did the *list* of models change? | did a model's *bytes* change? |
| Signals | file mtime, HTTP ETag / Last-Modified, listing digest | commit SHA, LFS content id, tree fingerprint |
| Cost | cheap | one or more network round trips |
| Today | not modeled at all | `update_available`, set by `check_for_model_updates` (L1912) |
| UX | "N new models available" | "N models have updates" |

`update_available` today means "the repository's commit advanced." There is no channel for "a
catalog gained an entry," because a catalog only changes today by the user pulling something, and
`user_models.json` mtime is not consulted. Once a URL catalog can gain entries on its own, these
are two different events with two different UIs and two different costs. Model them as two:
`enumeration_changed()` on the catalog, `compare()` on the repository, and keep the `update_available`
flag as the artifact-level one so existing clients are unaffected.

---

## 6. The smart router

### 6.1 The naming collision is most of the confusion

Two unrelated things are called "router":

- **`lemon::Router`** (`router.cpp`, 2,966 lines) — subprocess pools, LRU residency, NPU
  exclusivity, watchdog reload, `chat_completion` / `embeddings` / `reranking` / `classify`
  dispatch. It is a **model instance runtime** and contains no policy.
- **The routing policy engine** (`routing_policy.h/.cpp`, parser, store, classifier services,
  decision response) — the actual smart router.

Renaming the first is a mechanical change across roughly 40 translation units and removes a large
share of the "it is spread everywhere" feeling on its own.

### 6.2 What is already right and should not move

`routing_policy.h` states an invariant and honors it: it includes only the standard library and
`nlohmann/json`, never a backend or `Router` header; backends are reachable only through
`ClassifierServices` / `CostServices` structs of injected `std::function`s. The engine does pure
model selection — boolean rules, first-match-wins, fail-open to `default_model` — and its header
comment already reserves trust, verdicts, audit, and consent as layers the engine never interprets.
15 test files cover it, including a conformance corpus.

**Conclusion: keep routing spread across layers, but with named components and a single
orchestration owner.** Consolidating it into one `SmartRouter` class would recreate the
`ModelManager` problem this document exists to fix.

### 6.3 Where it is actually smeared

| Layer | Today | Assessment |
|---|---|---|
| Policy schema / parse | `routing_policy_parser.cpp` (678) | fine |
| Compile + evaluate | `routing_policy.cpp` (1,451) | fine, pure |
| Persistence + hot reload | `routing_policy_store.cpp` (180) | **unwired — no references outside its own files** |
| Capability adapters | `routing_classifier_services.cpp` (568) | binds to a concrete `Router&` |
| **Per-request decision** | **`Server::route_collection_request`** (`server.cpp:3670`) | orchestration lives in the HTTP layer |
| Decision result type | `RouterDispatchResult` declared in **`server.h:43`** | routing vocabulary in the HTTP header |
| Session affinity | `Router::note_route_decision` (`router.cpp:2495`) | see finding 2 |
| Helper-model residency | `Router::reconcile_routing_helpers` (`:404`) + `Server::active_policy_helper_models` (`:3857`) | policy question inside the runtime |
| Execution | `Router::chat_completion` etc. | right home, wrong class name |
| Presentation | `route_decision_response.cpp`, `prometheus_metrics.cpp` | fine |

Four findings worth acting on:

**1. Every routed request recompiles the entire policy AST.** `server.cpp:3686-3694` copies
`RoutePolicy` and constructs a fresh `RoutingPolicyEngine` per request. That constructor
(`routing_policy.cpp:1404`) compiles every rule's match expression — including regex leaves — with
the comment "compile every rule's match expression once, at construction, so `route()` does pure
tree-walking." Construction happens per request. Meanwhile `RoutingPolicyStore`, written for
precisely this problem with an atomic immutable snapshot and a passing `test_routing_policy_store.cpp`,
is referenced by nothing outside its own two files; its header still says
*"not wired into Server request dispatch yet: #2385 owns attaching a store/registry to the live
server lifecycle."* Wiring it is low risk, and the perf win is immediate.

**2. Session affinity does not exist.** `route_last_target_` (`router.h:303`) is written only
inside `note_route_decision` (`router.cpp:2495-2519`), where the lookup serves solely to detect a
*change* and bump `routing_switches_total_`. Nothing reads it to pin a conversation to a model.
The name implies behavior that is not there.

**3. `active_policy_helper_models()` deep-copies the whole store.** It calls
`model_manager_->get_supported_models()`, which copies every `ModelInfo` including `GgufMetadata`,
on every registry change, purely to project `route_policy->helper_models` into a set
(`server.cpp:3857-3870`, called from `:410` and `:422`). A store snapshot carrying a precomputed
helper set deletes this function, and per-catalog change events from the `SourceChangeBus` mean the
recompute only runs when a catalog that can contain a router collection actually changed.

**4. Helper reconciliation is split three ways.** `ModelManager` holds the policies, a `Server`
lambda computes the needed set (`server.cpp:409-410`), and `Router` executes
(`reconcile_routing_helpers` `:404`, `apply_routing_helper_reconcile` `:415`,
`prune_stale_routing_helpers_locked` `:447`, `reclaim_or_defer_helper_locked` `:467`,
`reclaim_stale_helper_if_idle` `:486`, `is_needed_helper_locked` `:547`). The *policy* question
("who is still needed?") belongs with routing; the *runtime* question ("can I reclaim this right
now?") belongs with the instance pool.

### 6.4 Proposed shape

```
RoutingService                       (new: the missing orchestration owner)
  owns: RoutingPolicyStore + hot reload, driven by store-change generations
  plan(request, store) -> Decision               <- Server::route_collection_request moves here
  candidate eligibility: which models a policy may select
  helper_requirements(store) -> set              <- replaces active_policy_helper_models()
  hooks: RoutePlanObserver, CandidateSet provider

RoutingPolicyEngine                  unchanged: pure, still stdlib + json only
ClassifierServices / CostServices    unchanged seams, rebound to ModelRuntime instead of Router
ModelRuntime  (was: Router)          pools / LRU / NPU exclusivity / eviction / dispatch,
                                     zero routing vocabulary
RouteDecisionResponse                unchanged presentation
CollectionOrchestrator               sibling use case over the same runtime port
```

**On hooks:** do not invent a plugin mechanism. The seam idiom already exists and is documented as
the engine's design north star. Two hook points are enough:

1. **`RoutePlanObserver`** — post-decision concerns (metrics, audit, the consent/trust layering the
   header explicitly reserves). The engine never interprets what observers do.
2. **`CandidateSet` provider** — "which models are eligible" is currently implicit in store
   filtering plus helper availability, and it will be the first thing any new routing feature needs
   to touch.

With dynamic loading off the table, both are just vectors of `std::function`.

### 6.5 Sync and notification: three names, two components

Applying one criterion — separate only if the threading model differs or there is a second
consumer — to `ChangeNotifier` / `UpdateChecker` / `ModelSyncService`:

- **`SourceChangeBus`: keep, re-scoped to catalogs.** With catalogs as instances, the unit of change
  becomes *which catalog or repository changed*, so events carry `{catalog_id | repository_id,
  generation}`. That turns the full-store recompute in finding 3 into a targeted one. Keeps the
  existing monotonic generation counter and the rule that callbacks fire outside all locks.
- **`UpdateChecker`: not a component.** Its only consumers are startup (`server.cpp:701`) and the
  sync endpoints (`:2658`, `:2763`) — no second consumer. Once `compare()` lives on the repository,
  the entire remaining body is: select targets, group by `(repository, revision)`, ask each
  repository, aggregate flags. Roughly 80 lines of pure orchestration. It becomes
  `ModelSyncService::scan_for_updates()`, still callable standalone at startup.
- **`ModelSyncService`: keep.** It is the only thing with threads, a queue, cancellation, and an
  SSE-visible state machine.

---

## 7. Migration plan

Every stage is independently shippable. Stages 1, 1.5 and 2 are safe to interleave with feature
work.

| # | Stage | Risk | Depends on |
|---|---|---|---|
| **0** | Characterization tests: golden output of `build_cache` for a fixture registry, plus pull-path tests. Convert the six test-only seams into real seams as they are replaced. | none | — |
| **1** | Extract `RecipeOptionsStore`. Own file, own mutex, near-zero coupling, roughly 400 lines out. Includes the ctor `builtin.` prefix migration (L1006-1050). | low | — |
| **1.5** | Extract `ModelCatalog` + `Locator`; instantiate `JsonCatalog` for the three existing JSON documents. Mechanical: three call sites, one shared parse primitive (`load_optional_json`, L1645), per-catalog `required()` reproducing today's throw-vs-degrade policies. No behavior change. | low | — |
| **2** | Extract `ArtifactTransfer` + typed `DownloadPlan` replacing the manifest JSON, preserving the contract in §4.3. | low | — |
| **3** | `ModelStore` + `StoreSnapshot` + `StoreBuilder` stages. Add `ScanCatalog` and `ListedCatalog` so all five sources sit behind one interface. Kill the mutable filter side tables and the hot-mutex read path. | medium | 1, 1.5 |
| **3.5** | Wire `RoutingPolicyStore`; remove per-request engine construction; move `RouterDispatchResult` out of `server.h`. | **low** | — |
| **4** | Split the axes: `RegistryId` + repository registry, capability flags, repository-owned list / metadata / revision / staleness (delete the duplicated HF fetch path at L5348-5444), repository-owned cache-dir naming, `ArtifactRef` provenance, locality-token minting rule, extend `is_reserved_registration_name`, split catalog staleness from artifact staleness (§5.3). Plus the artifact role split (§4.6): `ArtifactSource` / `FetchableRepository` / `ArtifactSourceResolver`, deleting the `info.source` switch (L1590-1624, `server.cpp:2954-2959`) and all five `npu_cache` skip sites. | medium-high | 3 |
| **5** | `ModelSyncService` with `scan_for_updates()` (absorbing `check_for_model_updates`) and `SourceChangeBus` keyed by catalog / repository id. | medium | 3, 4 |
| **6** | `ModelIngest` + `CollectionResolver`; invert `BackendOps::download_model` / `is_downloaded` into `DownloadDirective`. Make selection per checkpoint (§4.6): migrate `WhisperServer::download_npu_compiled_cache` onto a second checkpoint source rather than a backend-side fetch, and give local / imported / cloud models explicit `Deletion` behavior. | high | 2, 4 |
| **7** | Delete the `ModelManager` facade; inject `ModelStore` / `ModelIngest` / `RecipeOptionsStore` into `server.cpp`, `ollama_api.cpp`, `mcp_server.cpp`, `anthropic_api.cpp`. | medium | 3-6 |
| **8** | Extract `RoutingService`; move `route_collection_request` and helper-requirement computation; rebind classifier seams to the runtime interface. | medium | 3, 3.5 |
| **9** | Rename `Router` -> `ModelRuntime` (~40 translation units). Resolve the affinity question (§9, Q1). | medium | 8 |

**Ordering notes.**
- Routing consolidation must follow the store snapshot, because policy population, helper
  projection, and candidate eligibility all read the store.
- Stage 3.5 is independent of everything and is listed early because it is the cheapest real
  performance win in the document.
- Remote catalogs by URL become a **feature built on Stage 1.5 plus §5.3**, not a refactor in this
  plan. Once `HttpLocator` and catalog staleness exist, the work is: config entries, ETag caching,
  and a "new models available" surface. Its open design questions are in §9, Q5.

### Stage 6 handling instructions

`download_model` (L4734-5499) contains load-bearing invariants that must move verbatim and be
changed, if at all, in a separate commit:

- early collection registration with rollback on component-resolution failure (L4899, L4975-4978);
- a cycle guard deliberately scoped to the current recursion stack so a legitimate DAG such as
  `A -> {B, C} -> D` is not misread as a cycle (L5040-5042);
- persistence ordering: registration and recipe options are saved **before** the
  `do_not_upgrade` cache shortcut returns (L5060-5065), which is what makes idempotent re-pulls and
  imports work;
- progress-callback wrapping so recursive per-component downloads emit one terminal `complete`
  event to the SSE stream (L4989-4998).

---

## 8. Test safety net and acceptance criteria

Existing coverage is good and should be treated as the contract:

- **Routing:** 15 files, including `test_routing_policy_engine`, `test_routing_policy_evaluator`,
  `test_routing_policy_deterministic`, `test_routing_policy_llm_router`,
  `test_routing_policy_registry`, `test_routing_conformance_corpus`, `test_routing_policy_store`
  (currently testing unwired code), `test_routing_helper_reconcile`, `test_route_decision_response`,
  `fake_classifier_services.h`.
- **Model manager:** `test_model_manager_collection_validation`, `test_model_download_state`,
  `test_model_sync_and_auto_update`, `test_extra_model_discovery`, `test_model_registry`,
  `test_hf_update_check`, `test_recipe_hiding`, `test_streaming_memory_filter`,
  `test_config_default_source` (9 files construct or exercise `ModelManager`).

Gaps to close in Stage 0: no characterization test pins the *whole* store output; `delete_model`,
`cleanup_orphaned_cache`, and `list_model_files` have no direct coverage.

**Acceptance criteria for the extensibility goal** — these are the tests that prove the axis split
is real rather than cosmetic:

1. A **third `FetchableRepository`** implemented against a local HTTP fixture, requiring no edit to
   any existing file except its own registration line. If the exercise requires touching the store,
   the sync service, or `ModelInfo`, the cut is wrong.
2. A **second `JsonCatalog` from a fixture path** appearing in the merged view with correct
   precedence against a built-in of the same bare name.
3. A **catalog-staleness** test that adds an entry to a JSON file and observes a
   `{catalog_id, generation}` event with no repository network call.
4. An ID round-trip test asserting every catalog's minted IDs survive `parse_canonical_id` and
   `is_reserved_registration_name` (§5.2).
5. A **`FilesystemRepository`** serving a fixture directory and pulling a model through it
   with **no HTTP at all** and no edit outside its own file. If it requires forking
   `TransferEngine`, §4.3 is wrong.
6. A model with **two checkpoints from different sources** — for example `main` from a fixture
   repository and an `npu_cache`-style checkpoint from a filesystem repository — resolving,
   reporting presence, and deleting independently. This is the test that per-checkpoint selection is
   real, and it fails against today's code, which skips that checkpoint type in five places.

---

## 9. Open questions requiring input from other owners

Recorded rather than answered. **No stage above is blocked on any of them** — each has a stated
default so work can proceed, and the defaults were chosen to be cheap to reverse.

**Q1. Is session affinity intended behavior?**
`note_route_decision` looks like a half-built feature: it maintains a fingerprint LRU of 1,024
entries that nothing reads. If affinity is intended, the first `RoutePlanObserver` consumes it and
the fingerprint LRU moves into `RoutingService`. If it only ever existed to feed
`routing_switches_total_`, it should be renamed to something honest, e.g. `RouteTelemetry`.
*Default assumption: telemetry only. Stage 8 moves it, Stage 9 renames it.* Reversing this later
means adding an observer, not undoing anything.

**Q2. Is editing a live `collection.router` policy without a restart a supported user flow?**
`RoutingPolicyStore`'s directory watcher and the `models_changed_callback` ->
`reconcile_routing_helpers` path both assume it is, but nothing currently lets a policy change take
effect without a store rebuild. If hot editing is intended, the store becomes the source of truth
and `ModelInfo::route_policy` becomes a projection of it — a tidier end state than two places
holding compiled policy state. If it is not intended, the watcher can be dropped and the store
reduces to a compiled-engine cache keyed by store generation.
*Default assumption: compiled-engine cache; the watcher stays but is not relied on.* Either way
Stages 3.5 and 8 proceed unchanged; only ownership of `route_policy` differs, behind one accessor.

**Q3. Who owns changes to `is_reserved_registration_name`?**
It is a security-relevant guard and Stage 4 modifies it. Flagging for review by whoever owns model
ID semantics rather than assuming.

**Q4. Is there an existing consumer of these identity strings outside this repository?**
The MCP gateway, the desktop app, and the web app all read route decisions and model names.
`RemoteRegistrySource` is persisted to disk in two files. Before changing either identity type, it
is worth a sweep for external integrations that parse these strings, since §4.1 and §5.1 both
depend on string stability across releases.

**Q5. What are the trust and security rules for remote catalogs added by URL?**
Not a blocker for this refactor, but it is the feature the `ModelCatalog` axis exists to enable, and
the answers shape `HttpLocator`. Needed before that work starts: whether URL catalogs are
allow-listed or user-arbitrary; whether they may shadow built-in names or only add new ones;
whether an entry from a remote catalog may register models without user confirmation; auth and
token handling for private catalogs; cache location, TTL, and offline behavior; and whether catalog
entries may name an arbitrary repository or only a constrained set. The §5.2 locality-token guard
handles ID collisions; it does not address provenance trust.

**Q6. May a single model's checkpoints come from different sources?**
§4.6 recommends per-checkpoint selection, and `npu_cache` already behaves as if they can. But
today's registration rules say the opposite on the *registry* dimension specifically:
`apply_default_pull_source` throws when a definition's checkpoints resolve to different registries
(`model_registry.h`), and `download_from_registry` inherits the model-level registry source for
auxiliary checkpoints (L5856). Two separate decisions are needed:

- *Source kind* per checkpoint (local vs registry vs none) — needed now; `npu_cache` cannot be
  expressed without it. Low risk, recommended.
- *Registry* per checkpoint (main from HF, aux from ModelScope) — a deliberate restriction today.
  Relaxing it changes `/pull` validation, `checkpoints_complete`, and update-check grouping, so it
  needs an owner decision on whether the restriction is intentional product policy or merely the
  path of least resistance.

*Default assumption: source kind per checkpoint yes, registry per checkpoint stays restricted.* The
interface supports both; only validation differs, so relaxing it later is a policy edit rather than
a redesign.

**Q7. What should deleting a local or imported model do?**
`delete_model` has no local-source case; a `local_path` model throws at the `models--` ancestor walk
(L6196) and the deletion never completes, though the user's files are left untouched. Options:
unregister only (leave bytes); unregister and confirm before deleting user-owned bytes; or treat
"imported into the cache" and "referenced in place" as different cases.
*Default assumption in this plan: unregister only, never delete bytes the user owns* — the safe
option, which Stage 6 implements. Confirm before shipping any behavior change here, since today's
throw is at least visible to the user.

---

## 10. Risks

- **Churn against active feature branches.** Stages 3, 4, 6, and 7 all touch `server.cpp`, which is
  the busiest file in the repository. Sequencing and short-lived branches matter more than the
  design here.
- **The axis split touches `download_model` more than a pure extraction would.** Separating
  catalog-driven selection from repository protocol from transfer means editing the 613-line
  function rather than lifting it. Mitigated by §7's handling instructions and by Stage 6 landing
  alone.
- **Two providers now means two parallel fetch paths if (a) in §2.3 is not fully closed.** The
  duplicated HF metadata fetch must be deleted, not merely supplemented, or the drift bug survives
  the refactor with a third copy.
- **Per-checkpoint selection changes a persisted format.** `.lemonade_registry.json` records
  `processed_models[model_name] = {selection, snapshot_id}` per repository cache dir. Making
  selection and status per checkpoint means keying that by checkpoint type as well, which is a
  migration of a file already on user disks — the same class of one-shot migration as the
  `recipe_options.json` prefix move in the constructor (L1006-1050), and it needs the same care.
- **Naming collision on the word "catalog."** The UI already uses it for the model list
  (`ModelManager.tsx` `model-catalog-item`, `AddModelPanel.tsx`, `modelData.ts` "general model
  catalog"). Using `ModelCatalog` for the per-document source may need a different term, or an
  explicit note in the UI-facing docs, to avoid two meanings.
- **Hidden temporal coupling in `build_cache`.** The filter's side tables are populated during a
  store build and read later by `get_model_filter_reason` and `recipes_with_all_models_filtered`.
  Any reordering that separates those two reads breaks error messages, not correctness. Stage 3
  makes this explicit by moving both into the snapshot.
- **`ModelInfo` stays shared.** Splitting components while sharing a 40-field aggregate means a
  change to one component's data can still surprise another. Accepted as the cost of not doing a
  39-file struct rewrite in the same pass.
- **Perf regressions from abstraction layers on the download path.** Mitigated by keeping
  `ArtifactTransfer` a move of existing code rather than a rewrite, and by criterion 1 in §8
  catching accidental duplication of the transfer loop.

---

## Appendix: how the figures were measured

All line numbers refer to the working tree at the time of writing.

- File sizes: `wc -l` — `model_manager.cpp` 6,731; `server.cpp` 8,302; `router.cpp` 2,966; header 642.
- 56 public methods: declarations inside `class ModelManager` up to `private:` in the header.
- 41 methods used by `server.cpp`: distinct `model_manager_-><method>` call sites.
- 39 translation units: files including `lemon/model_manager.h` or `"model_manager.h"` under `src/` and `test/`.
- 65 lock acquisitions: occurrences of `lock_guard` / `unique_lock` in `model_manager.cpp`.
- Function sizes: distance to the next top-level definition.
- Eager loading of four documents: `model_manager.cpp:1002-1005` in the constructor.
- Three catalog-shaped loaders: `load_server_models` L1632 (resource path, throws),
  `load_optional_json` L1645 (path, degrades), `load_architecture_defaults` L1659 (resource path,
  warns); user file path from `get_user_models_file` L1055.
- `download_from_manifest` registry-agnostic: lines 5500-5818 contain no `registry`, provider,
  snapshot, or revision reference; keys consumed are `download_path`, `files_count`, and
  `files[{name, url, size, hash}]`.
- Repository-specific revision handling: `read_hf_ref_main` L268, `active_hf_snapshot_path` L326,
  `write_hf_ref_main` L336, used at L1204, L4449, L5179, L5920, L6036.
- Staleness special-cases: `source == RemoteRegistrySource::HuggingFace` at L2070 and L5936;
  ModelScope tree fingerprint at `model_registry.cpp:516`; `can_reuse_previous_hf_snapshot` L5308.
- Duplicated provider fetch: `hf_file_metadata_from_tree_file` L5348, `fetch_hf_file_metadata_for_ref`
  L5376 (reads `HF_ENDPOINT`, builds `/api/models/<repo>/tree/<ref>`).
- Cache-dir ternary: `model_registry.cpp:1028-1037`. Repository factory: `model_registry.cpp:1039`.
  Enum: `model_registry.h:15`. Providers: `model_registry.cpp:359`, `:442`.
- Inversion: `backends/backend_ops.cpp:114-126` calling `model_manager.cpp:2969` and `:2973`.
- Cloud name composition: `backends/cloud/cloud_server.cpp:250-275`, used at `:1075`.
- `check_for_model_updates` callers: `server.cpp:701` (startup), `:2658`, `:2763` (sync endpoints).
- `RoutingPolicyStore` unwired: `grep -rn RoutingPolicyStore src/cpp` returns only
  `routing_policy_store.{h,cpp}` and its test.
- `route_last_target_` unread: only occurrences are inside `Router::note_route_decision`
  (`router.cpp:2501-2515`) plus its declaration (`router.h:303`).
- Per-request engine construction: `server.cpp:3686-3694`; engine constructor that compiles rules:
  `routing_policy.cpp:1404`.
- Routing test files: 15 under `test/cpp` matching routing/classifier concerns (14 tests plus
  `fake_classifier_services.h`).
- GGUF magic-number rejection in the transfer phase: `model_manager.cpp:5622-5634`.
- UI "catalog" usages: `src/app/src/renderer/ModelManager.tsx:2087`,
  `src/app/src/renderer/AddModelPanel.tsx:281`, `src/app/src/renderer/utils/modelData.ts:40`.
- Artifact-source branches today: `ModelInfo::source` (header L102); `resolve_model_path`
  L1590-1624 (`local_path` as-is, `local_upload` rooted in the cache dir, else registry layout);
  `EXTRA_MODEL_SOURCE` constant L148 assigned at L1160; mixed-list validation at
  `server.cpp:2954-2959`; `type == "npu_cache"` skipped at L2951, L5188, L5860, L6237, L6292;
  `WhisperServer::download_npu_compiled_cache` `whispercpp_server.cpp:155` with its
  `resolve_checkpoint_path` override at `:638`; `checkpoint_looks_like_repo_id`
  `model_registry.cpp:864`; `backend_self_manages_downloads` L4345; cloud `downloaded = true` /
  `size = 0` `cloud_server.cpp:1082-1083`; `delete_model` throw at `model_manager.cpp:6196`.
- Build wiring: `model_manager.cpp` is compiled at `CMakeLists.txt:1011`.
