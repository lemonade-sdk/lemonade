use lemonade_context_engine_lib::indexing::store::{NewChunk, VectorStore};
use lemonade_context_engine_lib::lemonade::LemonadeClient;
use lemonade_context_engine_lib::rag;
use std::fs;
use std::sync::atomic::AtomicBool;
use std::time::Instant;

/// Decomposes the cold-index cost at 10k chunks into its two unknown parts, to find out
/// why per-file cost grows with corpus size (6.07 ms/file at 1k -> 10.50 ms/file at 10k)
/// instead of staying flat. Filesystem+hash cost is already known to be ~0.8s at this
/// size from the unchanged-rescan benchmark, so it is not re-measured here.
///
///   cargo test --test bench_decompose --locked -- --ignored --nocapture
const N: usize = 10_000;
const BATCH: usize = 64;
const DIM: usize = 768;

fn note_text(i: usize) -> String {
    format!(
        "# Note {i}\n\nThis is a short standalone note about topic {i}. \
         It has just enough text to produce a chunk or two, mimicking a repo full \
         of small files rather than a few huge ones.\n\n\
         Some more filler content unique to file {i}, referencing item number {}.\n",
        i * 7
    )
}

/// Pure Lemonade embedding cost for 10k short texts at the production batch size, with
/// no store, no filesystem, and no chunking in the loop.
#[tokio::test]
#[ignore]
async fn pure_embedding_cost() {
    let client = LemonadeClient::new("http://localhost:13305/v1".to_string());
    if !client.is_reachable().await {
        eprintln!("Lemonade not reachable, skipping");
        return;
    }

    let texts: Vec<String> = (0..N).map(note_text).collect();

    let start = Instant::now();
    let mut batches = 0usize;
    for batch in texts.chunks(BATCH) {
        client
            .embed(batch, rag::EMBEDDING_MODEL)
            .await
            .expect("embed failed");
        batches += 1;
    }
    let secs = start.elapsed().as_secs_f64();

    eprintln!(
        "\nPURE EMBEDDING: {N} texts in {batches} batches of {BATCH} -> {secs:.2}s \
         ({:.1} chunks/s, {:.1} ms/batch)",
        N as f64 / secs,
        secs * 1000.0 / batches as f64
    );
}

/// Store cost in isolation: the same 10k upserts and the same periodic full-index
/// flush cadence the indexer uses, with pre-made vectors so no network is involved.
/// Each flush re-serializes and rewrites the *whole* index, so if this cost grows
/// faster than linearly it is the source of the superlinear scaling.
#[tokio::test]
#[ignore]
async fn store_and_flush_cost() {
    let mut dir = std::env::temp_dir();
    dir.push(format!("lce-decompose-{}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();

    // Same shape as a real embedding: unit-ish f32 vector of the production dimension.
    let embedding: Vec<f32> = (0..DIM).map(|d| ((d % 17) as f32 - 8.0) / 8.0).collect();
    let live = AtomicBool::new(false);

    // Report at the same corpus sizes as Test A so the growth curve is comparable.
    for &limit in &[1_000usize, 5_000, 10_000] {
        let index_path = dir.join(format!("index-{limit}.bin"));
        let store = VectorStore::open(index_path.clone());
        store.reset_if_model_changed(rag::EMBEDDING_MODEL);

        let start = Instant::now();
        for i in 0..limit {
            let text = note_text(i);
            store
                .upsert_file(
                    &format!("D:\\corpus\\note-{i}.md"),
                    1,
                    i as u64,
                    vec![NewChunk {
                        start_line: 1,
                        end_line: 5,
                        text,
                        embedding: embedding.clone(),
                    }],
                    &live,
                )
                .expect("upsert failed");
            // Mirrors FLUSH_EVERY_N_FILES in rag.rs.
            if (i + 1) % 50 == 0 {
                store.flush().expect("flush failed");
            }
        }
        store.flush().expect("final flush failed");
        let secs = start.elapsed().as_secs_f64();

        eprintln!(
            "STORE+FLUSH: {limit:>6} files -> {secs:>7.2}s  ({:>6.3} ms/file)",
            secs * 1000.0 / limit as f64
        );
        let _ = fs::remove_file(&index_path);
    }

    let _ = fs::remove_dir_all(&dir);
}
