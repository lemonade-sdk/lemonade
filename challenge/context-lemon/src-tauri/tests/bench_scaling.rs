use lemonade_context_engine_lib::indexing::store::VectorStore;
use lemonade_context_engine_lib::lemonade::LemonadeClient;
use lemonade_context_engine_lib::rag;
use std::fs;
use std::path::PathBuf;
use std::sync::atomic::AtomicBool;
use std::time::Instant;

/// Manual, throwaway scaling benchmark. Not part of the regular suite — run with:
///   cargo test --test bench_scaling --locked -- --ignored --nocapture
///
/// Covers:
///   A. cold-index scaling at 1k / 5k / 10k small files (one chunk each)
///   C. incremental re-indexing on the 10k corpus: an unchanged rescan, then a 1%
///      change, to put a real number on the README's "unchanged content is not
///      re-embedded" claim.
fn make_corpus(dir: &PathBuf, count: usize) {
    let _ = fs::remove_dir_all(dir);
    fs::create_dir_all(dir).unwrap();
    for i in 0..count {
        let text = format!(
            "# Note {i}\n\nThis is a short standalone note about topic {i}. \
             It has just enough text to produce a chunk or two, mimicking a repo full \
             of small files rather than a few huge ones.\n\n\
             Some more filler content unique to file {i}, referencing item number {}.\n",
            i * 7
        );
        fs::write(dir.join(format!("note-{i}.md")), text).unwrap();
    }
}

async fn run_index(dir: &PathBuf, index_path: &PathBuf) -> (rag::IndexStats, f64) {
    let client = LemonadeClient::new("http://localhost:13305/v1".to_string());
    let store = VectorStore::open(index_path.clone());
    let cancel = AtomicBool::new(false);
    let start = Instant::now();
    let stats = rag::index_folder(&store, &client, &dir.to_string_lossy(), &cancel, &|_| {})
        .await
        .expect("index_folder failed");
    (stats, start.elapsed().as_secs_f64())
}

#[tokio::test]
#[ignore]
async fn cold_index_scaling() {
    let client = LemonadeClient::new("http://localhost:13305/v1".to_string());
    if !client.is_reachable().await {
        eprintln!("Lemonade not reachable, skipping");
        return;
    }

    let mut base = std::env::temp_dir();
    base.push(format!("lce-scaling-{}", std::process::id()));
    let _ = fs::remove_dir_all(&base);
    fs::create_dir_all(&base).unwrap();

    eprintln!("\n=== Test A: cold-index scaling ===");
    for count in [1_000usize, 5_000, 10_000] {
        let dir = base.join(format!("a-{count}"));
        let index_path = base.join(format!("a-{count}-index.bin"));
        make_corpus(&dir, count);
        let (stats, secs) = run_index(&dir, &index_path).await;
        eprintln!(
            "{count:>6} files -> {:>6} indexed / {:>6} chunks in {:>7.2}s  ({:>6.1} files/s, {:>6.1} chunks/s)",
            stats.files_indexed,
            stats.chunks_indexed,
            secs,
            stats.files_indexed as f64 / secs,
            stats.chunks_indexed as f64 / secs,
        );
        assert_eq!(stats.files_indexed, count);
        assert_eq!(stats.files_failed, 0);
        let _ = fs::remove_dir_all(&dir);
        let _ = fs::remove_file(&index_path);
    }

    let _ = fs::remove_dir_all(&base);
}

#[tokio::test]
#[ignore]
async fn incremental_reindexing() {
    let client = LemonadeClient::new("http://localhost:13305/v1".to_string());
    if !client.is_reachable().await {
        eprintln!("Lemonade not reachable, skipping");
        return;
    }

    let mut base = std::env::temp_dir();
    base.push(format!("lce-incremental-{}", std::process::id()));
    let _ = fs::remove_dir_all(&base);
    fs::create_dir_all(&base).unwrap();

    eprintln!("\n=== Test C: incremental re-indexing ===");
    const N: usize = 10_000;
    let dir = base.join("c-corpus");
    let index_path = base.join("c-index.bin");
    make_corpus(&dir, N);

    let (initial, initial_secs) = run_index(&dir, &index_path).await;
    eprintln!(
        "initial:        {N} files -> {} chunks in {:.2}s",
        initial.chunks_indexed, initial_secs
    );
    assert_eq!(initial.files_indexed, N);

    let (rescan, rescan_secs) = run_index(&dir, &index_path).await;
    eprintln!(
        "unchanged scan: {} skipped / {} re-embedded in {:.2}s",
        rescan.files_skipped_unchanged, rescan.files_indexed, rescan_secs
    );
    assert_eq!(rescan.files_indexed, 0, "an unchanged rescan must not re-embed anything");
    assert_eq!(rescan.files_skipped_unchanged, N);

    // Modify 1% of the corpus (100 of 10,000 files) and re-index.
    let changed = N / 100;
    for i in 0..changed {
        let text = format!("# Note {i} (edited)\n\nThis file's content changed for the 1% test.\n");
        fs::write(dir.join(format!("note-{i}.md")), text).unwrap();
    }
    let (partial, partial_secs) = run_index(&dir, &index_path).await;
    eprintln!(
        "1% changed:     {} re-embedded / {} skipped in {:.2}s",
        partial.files_indexed, partial.files_skipped_unchanged, partial_secs
    );
    assert_eq!(partial.files_indexed, changed);
    assert_eq!(partial.files_skipped_unchanged, N - changed);

    let _ = fs::remove_dir_all(&base);
}
