use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::time::SystemTime;

/// Bumped whenever the on-disk layout or the content-hash algorithm changes, so a
/// stale index is detected explicitly instead of silently mis-comparing hashes.
///
/// v2: embeddings are stored int8-quantised and chunk text lives in a side blob
/// rather than on the heap, to hold the resident index inside the memory budget.
/// v3: records the embedding model, and hashes content with FNV-1a instead of
/// `DefaultHasher`.
const STORE_FORMAT_VERSION: u32 = 3;

/// int8 range used for quantised embeddings. Vectors are unit-normalised first and
/// then rescaled so each one spans the full range: a 768-d unit vector has
/// components around 0.036, which would otherwise use barely three of the eight bits.
const QUANT_MAX: f32 = 127.0;

/// Rewrite the text blob once it is mostly dead bytes and large enough to be worth
/// the pass. Without this, re-indexing an edited folder grows the blob forever.
const COMPACT_MIN_BYTES: u64 = 8 * 1024 * 1024;
const COMPACT_GARBAGE_RATIO: u64 = 2;

/// A retrieval result, hydrated from metadata plus a read of the text blob.
#[derive(Debug, Clone)]
pub struct Chunk {
    pub file_path: String,
    pub start_line: usize,
    pub end_line: usize,
    pub text: String,
}

/// A chunk on its way into the store, still carrying full-precision text and vector.
#[derive(Debug, Clone)]
pub struct NewChunk {
    pub start_line: usize,
    pub end_line: usize,
    pub text: String,
    pub embedding: Vec<f32>,
}

/// Per-file fingerprint. Kept out of the chunk records so an unchanged-file check is
/// one lookup rather than a scan over every chunk.
#[derive(Debug, Serialize, Deserialize, Clone)]
struct FileEntry {
    path: String,
    mtime: u64,
    content_hash: u64,
}

/// The resident record for one chunk. Text is a (offset, len) pair into the blob;
/// the vector is int8. Roughly 820 bytes at 768 dimensions, against ~5 KB when the
/// text and f32 vector were both held on the heap.
#[derive(Debug, Serialize, Deserialize, Clone)]
struct ChunkMeta {
    file_idx: u32,
    start_line: u32,
    end_line: u32,
    text_offset: u64,
    text_len: u32,
    /// Multiplier that turns a quantised component back into a unit-vector component.
    scale: f32,
    vector: Vec<i8>,
}

#[derive(Debug, Serialize, Deserialize)]
struct StoreData {
    version: u32,
    /// Which model produced these vectors. Two models can share a dimensionality
    /// while embedding into completely different spaces, so the dimension check
    /// alone would let a model swap through and silently poison every ranking.
    embedding_model: String,
    /// Embedding dimensionality, 0 while the index is empty.
    dim: usize,
    /// Blob files are generation-numbered so compaction can publish a new one and
    /// only drop the old after the index that references it is safely on disk.
    blob_generation: u64,
    /// Authoritative blob length. The file itself may be longer after a crash mid-append;
    /// those trailing bytes are unreferenced and get truncated on the next write.
    blob_len: u64,
    files: Vec<FileEntry>,
    chunks: Vec<ChunkMeta>,
}

impl Default for StoreData {
    fn default() -> Self {
        StoreData {
            version: STORE_FORMAT_VERSION,
            embedding_model: String::new(),
            dim: 0,
            blob_generation: 0,
            blob_len: 0,
            files: Vec::new(),
            chunks: Vec::new(),
        }
    }
}

struct Inner {
    data: StoreData,
    /// Path → index into `data.files`, so `file_fingerprint` is O(1) instead of a
    /// scan per file (which made a full index run quadratic in the file count).
    by_path: HashMap<String, u32>,
    /// Lazily opened read/write handle on the current blob generation.
    blob: Option<fs::File>,
    /// Where the index lives; the blob is its sibling.
    index_path: PathBuf,
}

pub struct VectorStore {
    path: PathBuf,
    inner: Mutex<Inner>,
    dirty: AtomicBool,
    /// Set when an existing index file could not be read. Surfaced to the UI so a
    /// corrupt index is never silently swallowed and then overwritten.
    load_error: Mutex<Option<String>>,
}

impl VectorStore {
    pub fn open(path: PathBuf) -> Self {
        let mut load_error = None;
        let data = if path.exists() {
            match fs::read(&path) {
                Ok(bytes) => match bincode::deserialize::<StoreData>(&bytes) {
                    Ok(parsed) if parsed.version == STORE_FORMAT_VERSION => parsed,
                    Ok(parsed) => {
                        load_error = Some(format!(
                            "index at {} was written by format version {} (this build expects {}); it will be rebuilt",
                            path.display(),
                            parsed.version,
                            STORE_FORMAT_VERSION
                        ));
                        let backup = path.with_extension("bin.corrupt");
                        let _ = fs::rename(&path, backup);
                        StoreData::default()
                    }
                    Err(e) => {
                        load_error = Some(format!(
                            "index at {} could not be read ({e}); a backup was kept and the index will be rebuilt",
                            path.display()
                        ));
                        // Preserve the unreadable file rather than overwriting it on the
                        // next save — the embeddings in it cost real compute.
                        let backup = path.with_extension("bin.corrupt");
                        let _ = fs::rename(&path, backup);
                        StoreData::default()
                    }
                },
                Err(e) => {
                    load_error = Some(format!(
                        "index at {} could not be opened: {e}",
                        path.display()
                    ));
                    StoreData::default()
                }
            }
        } else {
            StoreData::default()
        };

        // Drop blob generations the loaded index does not reference. This also clears
        // the previous generation's file after a rebuild, so a format bump does not
        // strand its text blob on disk forever.
        gc_stale_blobs(&path, data.blob_generation);

        let by_path = build_path_map(&data.files);
        VectorStore {
            path: path.clone(),
            inner: Mutex::new(Inner {
                data,
                by_path,
                blob: None,
                index_path: path,
            }),
            dirty: AtomicBool::new(false),
            load_error: Mutex::new(load_error),
        }
    }

    pub fn take_load_error(&self) -> Option<String> {
        self.load_error.lock().unwrap().take()
    }

    /// Wipe the index when the embedding model changes. Vectors from a different
    /// model are not comparable with the new ones, so keeping them would leave
    /// retrieval silently ranking against a foreign space. Returns true if it reset.
    pub fn reset_if_model_changed(&self, model: &str) -> bool {
        let mut guard = self.inner.lock().unwrap();
        if guard.data.embedding_model == model {
            return false;
        }
        let had_content = !guard.data.chunks.is_empty();
        guard.data.embedding_model = model.to_string();
        if !had_content {
            drop(guard);
            self.dirty.store(true, Ordering::SeqCst);
            return false;
        }
        guard.data.dim = 0;
        guard.data.files.clear();
        guard.data.chunks.clear();
        // Keep the generation and reset the length: `ensure_blob` truncates the
        // existing file, so no orphaned blob is left behind.
        guard.data.blob_len = 0;
        guard.by_path.clear();
        guard.blob = None;
        drop(guard);
        self.dirty.store(true, Ordering::SeqCst);
        true
    }

    /// Replace one file's chunks and record its fingerprint. Text is appended to the
    /// blob immediately (so it never accumulates on the heap); the index itself is
    /// only persisted by [`VectorStore::flush`].
    ///
    /// `cancel` is checked *under the store lock*, which is what makes removal safe:
    /// `remove_folder` sets the flag and then purges while holding the same lock, so
    /// an in-flight index job can never slip a write in after its folder was purged.
    /// Returns false when the write was skipped because the job was cancelled.
    pub fn upsert_file(
        &self,
        file_path: &str,
        mtime: u64,
        content_hash: u64,
        new_chunks: Vec<NewChunk>,
        cancel: &AtomicBool,
    ) -> std::io::Result<bool> {
        if new_chunks.is_empty() {
            self.clear_file(file_path);
            return Ok(true);
        }

        let mut guard = self.inner.lock().unwrap();
        if cancel.load(Ordering::SeqCst) {
            return Ok(false);
        }

        let dim = new_chunks[0].embedding.len();
        if guard.data.dim == 0 {
            guard.data.dim = dim;
        } else if guard.data.dim != dim {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                format!(
                    "embedding dimension changed from {} to {dim}; delete the index and re-index",
                    guard.data.dim
                ),
            ));
        }

        // One buffer, one write syscall per file, and the text is visible to readers
        // as soon as the call returns (no user-space buffering to flush first).
        let mut payload = Vec::new();
        let base = guard.data.blob_len;
        let mut pending = Vec::with_capacity(new_chunks.len());
        for c in &new_chunks {
            if c.embedding.len() != dim {
                return Err(std::io::Error::new(
                    std::io::ErrorKind::InvalidInput,
                    "chunks in one file produced embeddings of differing lengths",
                ));
            }
            let offset = base + payload.len() as u64;
            payload.extend_from_slice(c.text.as_bytes());
            let (vector, scale) = quantize(&c.embedding);
            pending.push(ChunkMeta {
                file_idx: 0, // fixed up below, once the file slot is known
                start_line: c.start_line as u32,
                end_line: c.end_line as u32,
                text_offset: offset,
                text_len: c.text.len() as u32,
                scale,
                vector,
            });
        }

        guard.append_blob(&payload)?;

        let file_idx = match guard.by_path.get(&path_key(file_path)).copied() {
            Some(idx) => {
                let entry = &mut guard.data.files[idx as usize];
                entry.mtime = mtime;
                entry.content_hash = content_hash;
                guard.data.chunks.retain(|c| c.file_idx != idx);
                idx
            }
            None => {
                let idx = guard.data.files.len() as u32;
                guard.data.files.push(FileEntry {
                    path: file_path.to_string(),
                    mtime,
                    content_hash,
                });
                guard.by_path.insert(path_key(file_path), idx);
                idx
            }
        };

        for mut meta in pending {
            meta.file_idx = file_idx;
            guard.data.chunks.push(meta);
        }

        drop(guard);
        self.dirty.store(true, Ordering::SeqCst);
        Ok(true)
    }

    /// Drop a file's chunks *and* its fingerprint. Dropping the fingerprint is the
    /// point: an unreadable or emptied file must not stay recorded as "unchanged",
    /// or it would never be retried.
    pub fn clear_file(&self, file_path: &str) {
        let mut guard = self.inner.lock().unwrap();
        let target = path_key(file_path);
        if !guard.by_path.contains_key(&target) {
            return;
        }
        guard.drop_files(|p| path_key(p) == target);
        drop(guard);
        self.dirty.store(true, Ordering::SeqCst);
    }

    /// Drop chunks under `folder` that are not also covered by one of
    /// `still_configured`, so removing a parent folder does not blow away a nested
    /// folder the user still watches.
    pub fn remove_folder(&self, folder: &str, still_configured: &[String]) -> std::io::Result<()> {
        {
            let mut guard = self.inner.lock().unwrap();
            guard.drop_files(|path| {
                is_within_folder(path, folder)
                    && !still_configured
                        .iter()
                        .any(|other| is_within_folder(path, other))
            });
        }
        self.dirty.store(true, Ordering::SeqCst);
        self.flush()
    }

    /// Purge files under `folder` that the latest walk no longer yields — deleted,
    /// renamed, newly ignored, or grown past the size cap. Returns how many were purged.
    pub fn retain_seen_files(&self, folder: &str, seen: &HashSet<String>) -> usize {
        let seen_keys: HashSet<String> = seen.iter().map(|p| path_key(p)).collect();
        let mut guard = self.inner.lock().unwrap();
        let purged = guard.drop_files(|path| {
            is_within_folder(path, folder) && !seen_keys.contains(&path_key(path))
        });
        drop(guard);
        if purged > 0 {
            self.dirty.store(true, Ordering::SeqCst);
        }
        purged
    }

    pub fn file_fingerprint(&self, file_path: &str) -> Option<(u64, u64)> {
        let guard = self.inner.lock().unwrap();
        let idx = *guard.by_path.get(&path_key(file_path))?;
        let entry = &guard.data.files[idx as usize];
        Some((entry.mtime, entry.content_hash))
    }

    pub fn stats(&self) -> (usize, usize) {
        let guard = self.inner.lock().unwrap();
        (guard.data.files.len(), guard.data.chunks.len())
    }

    /// Approximate resident size of the index, for the memory budget claim in the
    /// README. Counts the quantised vectors, the metadata records and the interned
    /// paths — everything the store keeps on the heap.
    pub fn resident_bytes(&self) -> usize {
        let guard = self.inner.lock().unwrap();
        let per_chunk = std::mem::size_of::<ChunkMeta>() + guard.data.dim;
        let files: usize = guard
            .data
            .files
            .iter()
            // Counted twice: once in the entry, once as the lookup-map key.
            .map(|f| f.path.len() * 2 + std::mem::size_of::<FileEntry>() + 32)
            .sum();
        guard.data.chunks.len() * per_chunk + files
    }

    /// Score against the quantised vectors, then hydrate only the surviving top-k
    /// from the text blob.
    ///
    /// Errors if the stored vectors have a different dimensionality than the query —
    /// otherwise every score would tie at 0.0 and retrieval would silently degrade
    /// into "return the k oldest chunks" while still presenting them as citations.
    pub fn search(
        &self,
        query_embedding: &[f32],
        top_k: usize,
    ) -> Result<Vec<(Chunk, f32)>, String> {
        let mut guard = self.inner.lock().unwrap();
        if guard.data.chunks.is_empty() {
            return Ok(Vec::new());
        }
        if guard.data.dim != query_embedding.len() {
            return Err(format!(
                "embedding dimension mismatch: index holds {}-d vectors but the query \
                 embedding is {}-d. The embedding model changed — delete the index and re-index.",
                guard.data.dim,
                query_embedding.len()
            ));
        }

        // Normalising the query once turns each comparison into a plain dot product.
        let norm = query_embedding
            .iter()
            .map(|x| x * x)
            .sum::<f32>()
            .sqrt();
        if norm == 0.0 || !norm.is_finite() {
            return Err("query embedding has zero magnitude".to_string());
        }
        let query_unit: Vec<f32> = query_embedding.iter().map(|x| x / norm).collect();

        let mut scored: Vec<(usize, f32)> = guard
            .data
            .chunks
            .iter()
            .enumerate()
            .map(|(i, c)| (i, dot_quantized(&query_unit, &c.vector, c.scale)))
            .collect();
        scored.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(std::cmp::Ordering::Equal));
        scored.truncate(top_k);

        let mut out = Vec::with_capacity(scored.len());
        for (i, score) in scored {
            let (file_idx, start_line, end_line, offset, len) = {
                let c = &guard.data.chunks[i];
                (
                    c.file_idx as usize,
                    c.start_line as usize,
                    c.end_line as usize,
                    c.text_offset,
                    c.text_len,
                )
            };
            let file_path = guard.data.files[file_idx].path.clone();
            let text = guard
                .read_blob(offset, len)
                .map_err(|e| format!("could not read chunk text for {file_path}: {e}"))?;
            out.push((
                Chunk {
                    file_path,
                    start_line,
                    end_line,
                    text,
                },
                score,
            ));
        }
        Ok(out)
    }

    /// Persist only if something changed since the last flush. Indexing calls this
    /// periodically and once at the end rather than after every file, which keeps
    /// total bytes written linear in the corpus size instead of quadratic.
    pub fn flush(&self) -> std::io::Result<()> {
        if !self.dirty.swap(false, Ordering::SeqCst) {
            return Ok(());
        }
        let result = self.flush_inner();
        if result.is_err() {
            // Keep the dirty flag set so a later flush retries.
            self.dirty.store(true, Ordering::SeqCst);
        }
        result
    }

    fn flush_inner(&self) -> std::io::Result<()> {
        let mut guard = self.inner.lock().unwrap();

        // Compaction publishes a *new* blob generation and leaves the old one in place.
        // Only once the index pointing at the new generation is durably on disk is the
        // old file removed, so a crash at any point leaves a consistent pair.
        let retired = guard.maybe_compact()?;
        guard.sync_blob()?;

        write_index_atomically(&self.path, &guard.data)?;

        if let Some(old) = retired {
            let _ = fs::remove_file(old);
        }
        Ok(())
    }
}

impl Inner {
    /// Open the current blob for read/write, dropping any unreferenced tail left by a
    /// crash so appended offsets stay consistent with the index.
    fn ensure_blob(&mut self) -> std::io::Result<&mut fs::File> {
        if self.blob.is_none() {
            let path = blob_path_for(&self.index_path, self.data.blob_generation);
            if let Some(parent) = path.parent() {
                fs::create_dir_all(parent)?;
            }
            let file = fs::OpenOptions::new()
                .read(true)
                .write(true)
                .create(true)
                .open(&path)?;
            let actual = file.metadata()?.len();
            if actual != self.data.blob_len {
                file.set_len(self.data.blob_len)?;
            }
            self.blob = Some(file);
        }
        Ok(self.blob.as_mut().unwrap())
    }

    fn append_blob(&mut self, payload: &[u8]) -> std::io::Result<()> {
        if payload.is_empty() {
            return Ok(());
        }
        let at = self.data.blob_len;
        let file = self.ensure_blob()?;
        file.seek(SeekFrom::Start(at))?;
        file.write_all(payload)?;
        self.data.blob_len = at + payload.len() as u64;
        Ok(())
    }

    fn read_blob(&mut self, offset: u64, len: u32) -> std::io::Result<String> {
        if len == 0 {
            return Ok(String::new());
        }
        let file = self.ensure_blob()?;
        file.seek(SeekFrom::Start(offset))?;
        let mut buf = vec![0u8; len as usize];
        file.read_exact(&mut buf)?;
        String::from_utf8(buf)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))
    }

    fn sync_blob(&mut self) -> std::io::Result<()> {
        if let Some(file) = self.blob.as_mut() {
            file.sync_all()?;
        }
        Ok(())
    }

    /// Remove files matching `should_drop`, along with their chunks, remapping the
    /// remaining file indices. Returns how many files were dropped.
    fn drop_files<F: Fn(&str) -> bool>(&mut self, should_drop: F) -> usize {
        let mut remap: Vec<Option<u32>> = Vec::with_capacity(self.data.files.len());
        let mut kept: Vec<FileEntry> = Vec::with_capacity(self.data.files.len());
        let mut dropped = 0usize;
        for entry in std::mem::take(&mut self.data.files) {
            if should_drop(&entry.path) {
                remap.push(None);
                dropped += 1;
            } else {
                remap.push(Some(kept.len() as u32));
                kept.push(entry);
            }
        }
        if dropped == 0 {
            self.data.files = kept;
            return 0;
        }
        self.data.files = kept;
        self.data
            .chunks
            .retain_mut(|c| match remap[c.file_idx as usize] {
                Some(new_idx) => {
                    c.file_idx = new_idx;
                    true
                }
                None => false,
            });
        self.by_path = build_path_map(&self.data.files);
        dropped
    }

    /// Rewrite the blob without its dead bytes when it is mostly garbage. Returns the
    /// path of the retired generation, for the caller to delete after the index is saved.
    fn maybe_compact(&mut self) -> std::io::Result<Option<PathBuf>> {
        let live: u64 = self.data.chunks.iter().map(|c| c.text_len as u64).sum();
        let total = self.data.blob_len;
        if total < COMPACT_MIN_BYTES || total < live.saturating_mul(COMPACT_GARBAGE_RATIO) {
            return Ok(None);
        }

        let old_path = blob_path_for(&self.index_path, self.data.blob_generation);
        let new_generation = self.data.blob_generation + 1;
        let new_path = blob_path_for(&self.index_path, new_generation);

        let mut source = fs::File::open(&old_path)?;
        let mut target = fs::File::create(&new_path)?;
        let mut cursor = 0u64;
        // Chunk order is arbitrary, so this is a seek-per-chunk pass. It only runs when
        // the blob has gone mostly dead, which is rare.
        for meta in &mut self.data.chunks {
            source.seek(SeekFrom::Start(meta.text_offset))?;
            let mut buf = vec![0u8; meta.text_len as usize];
            source.read_exact(&mut buf)?;
            target.write_all(&buf)?;
            meta.text_offset = cursor;
            cursor += meta.text_len as u64;
        }
        target.sync_all()?;

        self.blob = None;
        self.data.blob_generation = new_generation;
        self.data.blob_len = cursor;
        Ok(Some(old_path))
    }
}

fn blob_path_for(index_path: &Path, generation: u64) -> PathBuf {
    let dir = index_path
        .parent()
        .map(|p| p.to_path_buf())
        .unwrap_or_else(|| PathBuf::from("."));
    dir.join(format!("chunks.{generation}.dat"))
}

/// Delete blob generations the index no longer points at.
fn gc_stale_blobs(index_path: &Path, keep_generation: u64) {
    let Some(dir) = index_path.parent() else {
        return;
    };
    let keep = blob_path_for(index_path, keep_generation);
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let Some(name) = path.file_name().and_then(|n| n.to_str()) else {
            continue;
        };
        if name.starts_with("chunks.") && name.ends_with(".dat") && path != keep {
            let _ = fs::remove_file(&path);
        }
    }
}

fn build_path_map(files: &[FileEntry]) -> HashMap<String, u32> {
    files
        .iter()
        .enumerate()
        .map(|(i, f)| (path_key(&f.path), i as u32))
        .collect()
}

/// Atomic write: serialize to a sibling temp file, fsync it, then rename over the
/// real index. A crash mid-write leaves the previous index intact.
fn write_index_atomically(path: &Path, data: &StoreData) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let bytes =
        bincode::serialize(data).map_err(|e| std::io::Error::new(std::io::ErrorKind::Other, e))?;

    let tmp = path.with_extension("bin.tmp");
    {
        let mut f = fs::File::create(&tmp)?;
        f.write_all(&bytes)?;
        f.sync_all()?;
    }
    fs::rename(&tmp, path)
}

/// Comparison key for a path. Windows paths are case-insensitive, so `D:\Docs` and
/// `D:\docs` are the same file and must not be indexed twice or half-removed. The
/// original spelling is kept for display; only comparisons use this.
pub fn path_key(path: &str) -> String {
    if cfg!(windows) {
        path.to_lowercase()
    } else {
        path.to_string()
    }
}

fn is_within_folder(file_path: &str, folder: &str) -> bool {
    // Path::starts_with compares whole components, so "D:\Work" does not match
    // "D:\Workshop".
    Path::new(&path_key(file_path)).starts_with(Path::new(&path_key(folder)))
}

/// Unit-normalise, then scale so the largest component lands on ±127. Returns the
/// quantised vector and the multiplier that reverses the scaling.
fn quantize(v: &[f32]) -> (Vec<i8>, f32) {
    let norm = v.iter().map(|x| x * x).sum::<f32>().sqrt();
    if norm == 0.0 || !norm.is_finite() {
        return (vec![0i8; v.len()], 0.0);
    }
    let max_abs = v
        .iter()
        .map(|x| (x / norm).abs())
        .fold(0.0f32, |a, b| if b > a { b } else { a });
    if max_abs == 0.0 {
        return (vec![0i8; v.len()], 0.0);
    }
    let scale = max_abs / QUANT_MAX;
    let quantised = v
        .iter()
        .map(|x| ((x / norm) / scale).round().clamp(-QUANT_MAX, QUANT_MAX) as i8)
        .collect();
    (quantised, scale)
}

/// Cosine similarity between a unit-length query and a quantised unit vector.
/// Both sides are unit-length, so the dot product *is* the cosine.
fn dot_quantized(query_unit: &[f32], quantised: &[i8], scale: f32) -> f32 {
    let mut dot = 0.0f32;
    for (q, &c) in query_unit.iter().zip(quantised.iter()) {
        dot += q * c as f32;
    }
    dot * scale
}

pub fn cosine_similarity(a: &[f32], b: &[f32]) -> f32 {
    if a.len() != b.len() || a.is_empty() {
        return 0.0;
    }
    let mut dot = 0.0f32;
    let mut norm_a = 0.0f32;
    let mut norm_b = 0.0f32;
    for i in 0..a.len() {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    if norm_a == 0.0 || norm_b == 0.0 {
        return 0.0;
    }
    dot / (norm_a.sqrt() * norm_b.sqrt())
}

pub fn hash_content(text: &str) -> u64 {
    // FNV-1a, spelled out rather than using DefaultHasher: std documents SipHash's
    // parameters as unspecified and free to change between Rust releases. A toolchain
    // upgrade would then change every stored fingerprint at once and silently force a
    // full re-embed of every file. STORE_FORMAT_VERSION cannot catch that, because the
    // format did not change — only the compiler did.
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for byte in text.as_bytes() {
        hash ^= *byte as u64;
        hash = hash.wrapping_mul(0x100_0000_01b3);
    }
    hash
}

pub fn unix_time(t: SystemTime) -> u64 {
    t.duration_since(SystemTime::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    const DIM: usize = 768;

    /// Deterministic pseudo-random source, so a failure is reproducible.
    struct Lcg(u64);
    impl Lcg {
        fn next_f32(&mut self) -> f32 {
            self.0 = self
                .0
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            ((self.0 >> 33) as f32 / (1u64 << 31) as f32) - 0.5
        }
        fn vector(&mut self, dim: usize) -> Vec<f32> {
            (0..dim).map(|_| self.next_f32()).collect()
        }
    }

    fn temp_index_path(tag: &str) -> PathBuf {
        let mut dir = std::env::temp_dir();
        dir.push(format!("lce-store-test-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir.join("index.bin")
    }

    fn chunk(text: &str, embedding: Vec<f32>) -> NewChunk {
        NewChunk {
            start_line: 1,
            end_line: 2,
            text: text.to_string(),
            embedding,
        }
    }

    /// Upsert with a live (uncancelled) job, which is what every test but the
    /// cancellation one wants.
    fn put(store: &VectorStore, path: &str, mtime: u64, hash: u64, chunks: Vec<NewChunk>) {
        let live = AtomicBool::new(false);
        assert!(store.upsert_file(path, mtime, hash, chunks, &live).unwrap());
    }

    /// The whole point of int8 is that it must not change *which* chunks come back.
    #[test]
    fn quantization_preserves_ranking_and_score() {
        let mut rng = Lcg(0x5EED);
        let query = rng.vector(DIM);
        let corpus: Vec<Vec<f32>> = (0..200).map(|_| rng.vector(DIM)).collect();

        let mut exact: Vec<(usize, f32)> = corpus
            .iter()
            .enumerate()
            .map(|(i, v)| (i, cosine_similarity(v, &query)))
            .collect();
        exact.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap());

        let qnorm = query.iter().map(|x| x * x).sum::<f32>().sqrt();
        let query_unit: Vec<f32> = query.iter().map(|x| x / qnorm).collect();
        let mut approx: Vec<(usize, f32)> = corpus
            .iter()
            .enumerate()
            .map(|(i, v)| {
                let (q, scale) = quantize(v);
                (i, dot_quantized(&query_unit, &q, scale))
            })
            .collect();
        approx.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap());

        assert_eq!(
            exact[0].0, approx[0].0,
            "quantisation changed the top hit: exact {:?} vs approx {:?}",
            &exact[..3],
            &approx[..3]
        );

        let exact_top: HashSet<usize> = exact[..5].iter().map(|(i, _)| *i).collect();
        let approx_top: HashSet<usize> = approx[..5].iter().map(|(i, _)| *i).collect();
        assert_eq!(
            exact_top.intersection(&approx_top).count(),
            5,
            "top-5 set changed: exact {exact_top:?} vs approx {approx_top:?}"
        );

        for ((_, e), (_, a)) in exact.iter().zip(approx.iter()) {
            assert!(
                (e - a).abs() < 0.01,
                "quantised score drifted too far: {e} vs {a}"
            );
        }
    }

    /// Text lives on disk now, so hydration has to survive a full close and reopen.
    #[test]
    fn survives_reopen_with_text_and_ranking_intact() {
        let path = temp_index_path("reopen");
        let mut rng = Lcg(7);
        let target = rng.vector(DIM);
        let other = rng.vector(DIM);

        {
            let store = VectorStore::open(path.clone());
            put(&store, "a.md", 11, 22, vec![chunk("the needle lives here", target.clone())]);
            put(&store, "b.md", 33, 44, vec![chunk("unrelated haystack", other)]);
            store.flush().unwrap();
        }

        let store = VectorStore::open(path);
        assert_eq!(store.stats(), (2, 2));
        assert_eq!(store.file_fingerprint("a.md"), Some((11, 22)));
        assert!(store.take_load_error().is_none());

        let hits = store.search(&target, 2).unwrap();
        assert_eq!(hits[0].0.file_path, "a.md");
        assert_eq!(hits[0].0.text, "the needle lives here");
        assert!(hits[0].1 > 0.9, "self-similarity should be ~1, got {}", hits[0].1);
    }

    /// Re-indexing a changed file must replace its chunks, not accumulate them.
    #[test]
    fn upsert_replaces_previous_chunks_for_the_file() {
        let path = temp_index_path("upsert");
        let mut rng = Lcg(9);
        let store = VectorStore::open(path);
        let v = rng.vector(DIM);

        put(&store, "a.md", 1, 1, vec![chunk("first", v.clone()), chunk("second", v.clone())]);
        assert_eq!(store.stats(), (1, 2));

        put(&store, "a.md", 2, 2, vec![chunk("rewritten", v.clone())]);
        assert_eq!(store.stats(), (1, 1));
        assert_eq!(store.file_fingerprint("a.md"), Some((2, 2)));

        let hits = store.search(&v, 5).unwrap();
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].0.text, "rewritten");
    }

    /// An unreadable file must lose its fingerprint too, or the next run would treat
    /// it as unchanged and never retry it.
    #[test]
    fn clear_file_drops_the_fingerprint() {
        let path = temp_index_path("clear");
        let mut rng = Lcg(11);
        let store = VectorStore::open(path);
        put(&store, "a.md", 5, 5, vec![chunk("text", rng.vector(DIM))]);
        assert!(store.file_fingerprint("a.md").is_some());

        store.clear_file("a.md");
        assert_eq!(store.file_fingerprint("a.md"), None);
        assert_eq!(store.stats(), (0, 0));
    }

    /// Removing a parent folder must not purge a nested folder the user still watches.
    #[test]
    fn remove_folder_spares_nested_configured_folders() {
        let path = temp_index_path("nested");
        let mut rng = Lcg(13);
        let store = VectorStore::open(path);
        let parent = if cfg!(windows) { "D:\\work" } else { "/work" };
        let nested = if cfg!(windows) { "D:\\work\\keep" } else { "/work/keep" };
        let outer = if cfg!(windows) { "D:\\work\\drop\\a.md" } else { "/work/drop/a.md" };
        let inner = if cfg!(windows) { "D:\\work\\keep\\b.md" } else { "/work/keep/b.md" };

        put(&store, outer, 1, 1, vec![chunk("outer", rng.vector(DIM))]);
        put(&store, inner, 1, 1, vec![chunk("inner", rng.vector(DIM))]);

        store.remove_folder(parent, &[nested.to_string()]).unwrap();

        assert_eq!(store.stats(), (1, 1));
        assert!(store.file_fingerprint(inner).is_some());
        assert_eq!(store.file_fingerprint(outer), None);
    }

    /// Files the walk no longer yields must stop being cited.
    #[test]
    fn retain_seen_files_purges_deleted_files() {
        let path = temp_index_path("retain");
        let mut rng = Lcg(17);
        let store = VectorStore::open(path);
        let folder = if cfg!(windows) { "D:\\docs" } else { "/docs" };
        let kept = if cfg!(windows) { "D:\\docs\\keep.md" } else { "/docs/keep.md" };
        let gone = if cfg!(windows) { "D:\\docs\\gone.md" } else { "/docs/gone.md" };

        put(&store, kept, 1, 1, vec![chunk("keep", rng.vector(DIM))]);
        put(&store, gone, 1, 1, vec![chunk("gone", rng.vector(DIM))]);

        let seen: HashSet<String> = [kept.to_string()].into_iter().collect();
        assert_eq!(store.retain_seen_files(folder, &seen), 1);
        assert_eq!(store.stats(), (1, 1));
    }

    /// A cancelled job must not be able to write, which is what makes removing a
    /// folder mid-index safe.
    #[test]
    fn cancelled_upsert_writes_nothing() {
        let path = temp_index_path("cancel");
        let mut rng = Lcg(23);
        let store = VectorStore::open(path);
        let cancelled = AtomicBool::new(true);

        let wrote = store
            .upsert_file("a.md", 1, 1, vec![chunk("nope", rng.vector(DIM))], &cancelled)
            .unwrap();

        assert!(!wrote, "a cancelled upsert must report that it skipped");
        assert_eq!(store.stats(), (0, 0));
        assert_eq!(store.file_fingerprint("a.md"), None);
    }

    /// Swapping the embedding model invalidates every vector; keeping them would rank
    /// the query against a foreign space while still presenting citations.
    #[test]
    fn changing_the_embedding_model_resets_the_index() {
        let path = temp_index_path("model");
        let mut rng = Lcg(29);
        let store = VectorStore::open(path);

        assert!(!store.reset_if_model_changed("model-a"), "first use is not a change");
        put(&store, "a.md", 1, 1, vec![chunk("text", rng.vector(DIM))]);
        assert_eq!(store.stats(), (1, 1));

        assert!(!store.reset_if_model_changed("model-a"), "same model must not reset");
        assert_eq!(store.stats(), (1, 1));

        assert!(store.reset_if_model_changed("model-b"), "a different model must reset");
        assert_eq!(store.stats(), (0, 0));
        assert_eq!(store.file_fingerprint("a.md"), None);
    }

    /// Windows paths are case-insensitive, so two spellings are one file.
    #[test]
    #[cfg(windows)]
    fn windows_paths_are_matched_case_insensitively() {
        let path = temp_index_path("case");
        let mut rng = Lcg(31);
        let store = VectorStore::open(path);
        let v = rng.vector(DIM);

        put(&store, "D:\\Docs\\Notes.md", 1, 1, vec![chunk("first", v.clone())]);
        // Same file, different spelling — must update in place, not add a second entry.
        put(&store, "d:\\docs\\notes.md", 2, 2, vec![chunk("second", v.clone())]);
        assert_eq!(store.stats(), (1, 1));
        assert_eq!(store.file_fingerprint("D:\\Docs\\Notes.md"), Some((2, 2)));

        // And a differently-cased folder still purges it.
        store.remove_folder("D:\\DOCS", &[]).unwrap();
        assert_eq!(store.stats(), (0, 0));
    }

    /// The fingerprint hash is persisted, so it must not depend on the toolchain.
    #[test]
    fn content_hash_is_stable_across_builds() {
        // FNV-1a of "hello" — a fixed expectation, so a change to hash_content that
        // would silently invalidate every user's index fails here first.
        assert_eq!(hash_content("hello"), 0xa430_d846_80aa_bd0b);
        assert_eq!(hash_content(""), 0xcbf2_9ce4_8422_2325);
        assert_ne!(hash_content("a"), hash_content("b"));
    }

    /// The memory budget is a headline claim, so hold it to a number.
    #[test]
    fn resident_footprint_stays_within_budget() {
        let path = temp_index_path("footprint");
        let mut rng = Lcg(19);
        let store = VectorStore::open(path);
        for i in 0..200 {
            put(
                &store,
                &format!("file-{i}.md"),
                1,
                1,
                vec![chunk(&"x".repeat(2000), rng.vector(DIM))],
            );
        }
        let per_chunk = store.resident_bytes() as f64 / 200.0;
        eprintln!(
            "resident: {per_chunk:.0} B/chunk → {:.0} MB at 50k chunks",
            per_chunk * 50_000.0 / 1_048_576.0
        );
        assert!(
            per_chunk < 1100.0,
            "expected well under ~1 KB resident per chunk, got {per_chunk:.0} B"
        );
    }
}
