use crate::indexing::store::path_key;
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

/// Tracks the in-flight index job per folder so one can be called off.
///
/// Without this, removing a folder mid-index left the spawned task happily writing
/// chunks for a folder that was no longer configured: the UI had no row left to
/// remove, yet `ask()` kept citing documents the user had explicitly un-shared.
#[derive(Default)]
pub struct IndexJobs {
    running: Mutex<HashMap<String, Arc<AtomicBool>>>,
}

impl IndexJobs {
    /// Register a new job for `folder`, superseding (and cancelling) any job already
    /// running for it, so re-adding a folder cannot leave two indexers racing.
    pub fn start(&self, folder: &str) -> Arc<AtomicBool> {
        let token = Arc::new(AtomicBool::new(false));
        let mut running = self.running.lock().unwrap();
        if let Some(previous) = running.insert(path_key(folder), Arc::clone(&token)) {
            previous.store(true, Ordering::SeqCst);
        }
        token
    }

    /// Signal the job for `folder` to stop. Safe to call when nothing is running.
    pub fn cancel(&self, folder: &str) {
        if let Some(token) = self.running.lock().unwrap().remove(&path_key(folder)) {
            token.store(true, Ordering::SeqCst);
        }
    }

    /// Deregister a finished job, but only if it is still the current one — a job that
    /// was already superseded must not clear its replacement's entry.
    pub fn finish(&self, folder: &str, token: &Arc<AtomicBool>) {
        let key = path_key(folder);
        let mut running = self.running.lock().unwrap();
        if running.get(&key).is_some_and(|current| Arc::ptr_eq(current, token)) {
            running.remove(&key);
        }
    }
}
