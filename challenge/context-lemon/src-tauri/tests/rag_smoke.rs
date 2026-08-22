use lemonade_context_engine_lib::rag;
use std::sync::atomic::AtomicBool;
use std::sync::Mutex;

mod common;

/// The indexing progress the UI shows ("N/M files scanned · K remaining") is only
/// meaningful if the callback actually fires and its counts are coherent, so assert
/// on the reported sequence rather than trusting that it compiles.
#[tokio::test]
async fn indexing_reports_progress_with_coherent_counts() {
    let Some(fixture) = common::setup().await else {
        return;
    };

    let sample = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../sample");
    let reported: Mutex<Vec<rag::IndexProgress>> = Mutex::new(Vec::new());
    let cancel = AtomicBool::new(false);

    rag::index_folder(
        &fixture.store,
        &fixture.client,
        &sample.to_string_lossy(),
        &cancel,
        &|p| reported.lock().unwrap().push(p),
    )
    .await
    .expect("index_folder failed");

    let reported = reported.into_inner().unwrap();
    assert!(!reported.is_empty(), "no progress was reported at all");

    let last = reported.last().unwrap();
    assert!(last.files_total > 0, "walk reported no files: {last:?}");
    assert_eq!(
        last.files_done, last.files_total,
        "the final progress report must account for every walked file: {last:?}"
    );

    // files_done must never run backwards or overshoot the total, or the UI would
    // show a count that jumps around or a negative "remaining".
    let mut previous = 0usize;
    for p in &reported {
        assert!(
            p.files_done >= previous,
            "files_done went backwards: {previous} then {}",
            p.files_done
        );
        assert!(
            p.files_done <= p.files_total,
            "files_done {} exceeded files_total {}",
            p.files_done,
            p.files_total
        );
        previous = p.files_done;
    }
}

/// Requires a running Lemonade server at localhost:13305. The fixture indexes the
/// bundled sample into disposable storage, keeping the test isolated from the user's
/// application index. It skips only when Lemonade itself is unreachable.
#[tokio::test]
async fn known_answer_questions_are_grounded_with_citations() {
    let Some(fixture) = common::setup().await else {
        return;
    };

    let cases = [
        ("What port does the Nightingale gateway listen on by default?", "7913"),
        ("Who is the lead engineer on Project Nightingale?", "Priya"),
        ("How long can a device be offline before Nightingale falls back to cloud inference?", "90"),
    ];

    for (question, expected_fact) in cases {
        let response = rag::ask(&fixture.store, &fixture.client, question)
            .await
            .unwrap_or_else(|e| panic!("ask() failed for {question:?}: {e}"));

        assert!(
            !response.sources.is_empty(),
            "expected at least one citation for {question:?}, got none"
        );
        assert!(
            response.answer.contains(expected_fact),
            "expected answer to {question:?} to mention {expected_fact:?}, got: {}",
            response.answer
        );
    }
}
