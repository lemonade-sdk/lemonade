//! Retrieval-quality checks against the bundled "Project Nightingale" corpus.
//!
//! `rag_smoke.rs` only asserts that an expected fact appears somewhere and that at
//! least one citation came back. That passes even if the answer came from the model's
//! prior rather than the corpus, and even if the citation points at the wrong file.
//! These tests close both gaps, and add the case that matters most for a RAG layer:
//! refusing to answer when the corpus does not contain the answer.

use lemonade_context_engine_lib::rag;

mod common;

fn cited_files(sources: &[rag::Source]) -> Vec<String> {
    sources
        .iter()
        .map(|s| {
            std::path::Path::new(&s.file)
                .file_name()
                .map(|n| n.to_string_lossy().to_string())
                .unwrap_or_else(|| s.file.clone())
        })
        .collect()
}

/// A fact that lives in exactly one file must be answered *and* attributed to that
/// file. Getting the number right while citing the wrong document is still a bug —
/// the citation is the product here.
#[tokio::test]
async fn answers_are_attributed_to_the_file_that_contains_the_fact() {
    let Some(fixture) = common::setup().await else {
        return;
    };

    // "30 seconds with no heartbeat -> dead" appears only in architecture.md.
    let response = rag::ask(
        &fixture.store,
        &fixture.client,
        "After how many seconds with no heartbeat is a Nightingale node marked dead?",
    )
    .await
    .expect("ask() failed");

    let files = cited_files(&response.sources);
    assert!(
        response.answer.contains("30"),
        "expected the 30-second figure, got: {}",
        response.answer
    );
    assert!(
        files.iter().any(|f| f == "architecture.md"),
        "expected architecture.md among citations, got {files:?} for answer: {}",
        response.answer
    );
}

/// Two facts that live in different files, so this only passes if retrieval pulled
/// more than one document and the model used both.
#[tokio::test]
async fn synthesises_facts_across_two_files() {
    let Some(fixture) = common::setup().await else {
        return;
    };

    let response = rag::ask(
        &fixture.store,
        &fixture.client,
        "What is the Talon Cache's default budget per accelerator, and which build introduced it?",
    )
    .await
    .expect("ask() failed");

    assert!(
        response.answer.contains("512"),
        "expected the 512 MB budget, got: {}",
        response.answer
    );
    assert!(
        response.answer.contains("0.4.0"),
        "expected build nightingale-0.4.0, got: {}",
        response.answer
    );
}

/// The one that separates a grounded RAG layer from a chatbot: the corpus says
/// nothing about budgets, so the only correct behaviour is to decline. A fabricated
/// figure here would be the single most damaging failure mode for this product.
#[tokio::test]
async fn declines_when_the_answer_is_not_in_the_corpus() {
    let Some(fixture) = common::setup().await else {
        return;
    };

    let response = rag::ask(
        &fixture.store,
        &fixture.client,
        "What was Project Nightingale's annual marketing budget in US dollars?",
    )
    .await
    .expect("ask() failed");

    let lower = response.answer.to_lowercase();
    let declined = [
        "not in the context",
        "not mentioned",
        "not specified",
        "not provided",
        "not included",
        "not available",
        "no information",
        "does not mention",
        "doesn't mention",
        "does not contain",
        "doesn't contain",
        "does not include",
        "doesn't include",
        "does not provide",
        "doesn't provide",
        "does not specify",
        "doesn't specify",
        "isn't mentioned",
        "is not mentioned",
        "cannot be determined",
        "can't be determined",
        "no mention",
    ]
    .iter()
    .any(|marker| lower.contains(marker));

    assert!(
        declined,
        "expected a refusal for an out-of-corpus question, got: {}",
        response.answer
    );

    // A refusal that still quotes a dollar figure is not a refusal.
    let fabricated_amount = response
        .answer
        .split('$')
        .skip(1)
        .any(|rest| rest.trim_start().chars().next().is_some_and(|c| c.is_ascii_digit()));
    assert!(
        !fabricated_amount,
        "refusal still contained a dollar amount: {}",
        response.answer
    );
}

/// Guards against the failure mode the dimension-mismatch bug used to cause: a
/// confident answer built from whatever chunks happened to rank first. The port is
/// 7913, and common defaults must not show up instead.
#[tokio::test]
async fn does_not_substitute_a_plausible_wrong_value() {
    let Some(fixture) = common::setup().await else {
        return;
    };

    let response = rag::ask(
        &fixture.store,
        &fixture.client,
        "What port does the Nightingale gateway listen on by default?",
    )
    .await
    .expect("ask() failed");

    // Printed under --nocapture so the documented example stays honest: if the shape
    // of a real answer drifts, it is visible here rather than only in the README.
    eprintln!("--- answer ---\n{}", response.answer);
    eprintln!("--- sources ---");
    for s in &response.sources {
        eprintln!("  {} (lines {}-{})", s.file, s.start_line, s.end_line);
    }

    assert!(
        response.answer.contains("7913"),
        "expected port 7913, got: {}",
        response.answer
    );
    for wrong in ["8080", "8000", "3000", "443", "80801"] {
        assert!(
            !response.answer.contains(wrong),
            "answer offered {wrong} as the port: {}",
            response.answer
        );
    }
}
