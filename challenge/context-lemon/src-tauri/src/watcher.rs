use crate::config::ConfigState;
use crate::indexing::store::path_key;
use crate::indexing::walker;
use notify::{RecommendedWatcher, RecursiveMode, Watcher};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{channel, Receiver, RecvTimeoutError, Sender};
use std::time::Duration;
use tauri::Manager;

/// How long the folder must go quiet before re-indexing. Editors do not write a file
/// once — they write, rename, and touch it several times over a few hundred
/// milliseconds, and a save-on-keystroke setup fires continuously. Re-indexing on the
/// first event would mean re-embedding a file the user is still typing into.
const QUIET_PERIOD: Duration = Duration::from_millis(1500);

/// How often to reconcile the watch list with the configured folders, so a folder
/// added or removed through the UI starts or stops being watched without a restart.
const SYNC_INTERVAL: Duration = Duration::from_secs(5);

/// Start watching configured folders for changes. Runs on its own OS thread: `notify`
/// delivers events on a background thread of its own, and the debounce loop below is
/// blocking, which has no business occupying an async runtime worker.
pub fn start(app: tauri::AppHandle) {
    std::thread::spawn(move || {
        if let Err(err) = run(app) {
            eprintln!("file watcher stopped: {err}");
        }
    });
}

fn run(app: tauri::AppHandle) -> notify::Result<()> {
    let (tx, rx): (Sender<PathBuf>, Receiver<PathBuf>) = channel();

    let mut watcher: RecommendedWatcher = notify::recommended_watcher(move |res| {
        let Ok(event) = res else { return };
        let event: notify::Event = event;
        for path in event.paths {
            // A rename or delete leaves nothing on disk to inspect, so filter on the
            // path alone rather than on file type.
            if walker::is_indexable_path(&path) {
                let _ = tx.send(path);
            }
        }
    })?;

    let mut watched: HashMap<String, String> = HashMap::new();
    // Folders that don't currently exist on disk (deleted, unplugged drive, or a
    // resource path that was valid at seed time but is gone after a rebuild). Without
    // this, a folder that can never be watched gets retried — and its failure
    // re-logged — every SYNC_INTERVAL, forever.
    let mut missing: HashSet<String> = HashSet::new();
    sync_watches(&app, &mut watcher, &mut watched, &mut missing);

    let mut pending: HashSet<PathBuf> = HashSet::new();
    loop {
        match rx.recv_timeout(SYNC_INTERVAL) {
            Ok(path) => {
                pending.insert(path);
                // Keep draining until the folder has been quiet for QUIET_PERIOD, so a
                // burst of saves collapses into one re-index.
                loop {
                    match rx.recv_timeout(QUIET_PERIOD) {
                        Ok(more) => {
                            pending.insert(more);
                        }
                        Err(RecvTimeoutError::Timeout) => break,
                        Err(RecvTimeoutError::Disconnected) => return Ok(()),
                    }
                }
                let folders = configured_folders(&app);
                for folder in affected_folders(&pending, &folders) {
                    let handle = app.clone();
                    tauri::async_runtime::spawn(async move {
                        // run_index_job supersedes any job already running for this
                        // folder, so rapid edits cannot pile up overlapping indexers.
                        crate::run_index_job(&handle, &folder).await;
                    });
                }
                pending.clear();
                sync_watches(&app, &mut watcher, &mut watched, &mut missing);
            }
            Err(RecvTimeoutError::Timeout) => {
                sync_watches(&app, &mut watcher, &mut watched, &mut missing);
            }
            Err(RecvTimeoutError::Disconnected) => return Ok(()),
        }
    }
}

fn configured_folders(app: &tauri::AppHandle) -> Vec<String> {
    app.state::<ConfigState>().0.lock().unwrap().folders.clone()
}

/// Map changed files back to the configured folders that contain them. Re-indexing is
/// per folder because the indexer is already incremental: unchanged files are skipped
/// on a content hash, so the cost of a folder pass is a walk, not re-embedding.
fn affected_folders(changed: &HashSet<PathBuf>, folders: &[String]) -> Vec<String> {
    let mut hit: HashMap<String, String> = HashMap::new();
    for path in changed {
        let path_str = path.to_string_lossy().to_string();
        for folder in folders {
            if Path::new(&path_key(&path_str)).starts_with(Path::new(&path_key(folder))) {
                hit.insert(path_key(folder), folder.clone());
            }
        }
    }
    hit.into_values().collect()
}

/// Add watches for newly configured folders and drop watches for removed ones.
/// Keyed by the normalised path but storing the original: `unwatch` must be given the
/// same path that was passed to `watch`, not a case-folded version of it.
fn sync_watches(
    app: &tauri::AppHandle,
    watcher: &mut RecommendedWatcher,
    watched: &mut HashMap<String, String>,
    missing: &mut HashSet<String>,
) {
    let folders = configured_folders(app);
    let desired: HashSet<String> = folders.iter().map(|f| path_key(f)).collect();

    for folder in &folders {
        let key = path_key(folder);
        if watched.contains_key(&key) {
            continue;
        }

        // A cheap existence check first, so a folder that's gone (deleted, unplugged
        // drive, a stale resource path from before a rebuild) costs one stat call per
        // sync instead of a failed watcher.watch() plus a repeated log line, forever.
        if !Path::new(folder).exists() {
            if missing.insert(key) {
                eprintln!("cannot watch {folder}: path does not exist (will retry if it appears)");
            }
            continue;
        }
        missing.remove(&key);

        match watcher.watch(Path::new(folder), RecursiveMode::Recursive) {
            Ok(()) => {
                watched.insert(key, folder.clone());
            }
            Err(err) => eprintln!("could not watch {folder}: {err}"),
        }
    }

    let stale: Vec<String> = watched
        .keys()
        .filter(|k| !desired.contains(*k))
        .cloned()
        .collect();
    for key in stale {
        if let Some(original) = watched.remove(&key) {
            let _ = watcher.unwatch(Path::new(&original));
        }
    }
    missing.retain(|k| desired.contains(k));
}
