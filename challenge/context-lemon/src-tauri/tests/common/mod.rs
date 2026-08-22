use lemonade_context_engine_lib::indexing::store::VectorStore;
use lemonade_context_engine_lib::lemonade::LemonadeClient;
use lemonade_context_engine_lib::rag;
use std::sync::atomic::AtomicBool;
use std::sync::{Mutex, MutexGuard, OnceLock};

static LIVE_TEST_LOCK: Mutex<()> = Mutex::new(());
static LIVE_TEST_DIR: OnceLock<tempfile::TempDir> = OnceLock::new();

/// A live Lemonade fixture backed by a disposable index of the bundled sample.
///
/// The guard serializes tests because local inference servers commonly process one
/// model request at a time. The temporary directory stays alive for the integration
/// test process so the sample only needs to be embedded once per test binary.
pub struct LiveFixture {
    pub store: VectorStore,
    pub client: LemonadeClient,
    _guard: MutexGuard<'static, ()>,
}

/// Returns `None` only when Lemonade is not running. Once the live precondition is
/// met, model or indexing failures are real test failures rather than silent skips.
pub async fn setup() -> Option<LiveFixture> {
    let guard = LIVE_TEST_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    let client = LemonadeClient::new("http://localhost:13305/v1".to_string());
    if !client.is_reachable().await {
        eprintln!("SKIPPED: Lemonade server not reachable at localhost:13305");
        return None;
    }

    let temp_dir = LIVE_TEST_DIR.get_or_init(|| {
        tempfile::tempdir().expect("failed to create a disposable live-test directory")
    });
    let store = VectorStore::open(temp_dir.path().join("index.bin"));

    if store.stats().1 == 0 {
        let sample = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../sample");
        assert!(sample.is_dir(), "sample corpus not found at {}", sample.display());

        let cancel = AtomicBool::new(false);
        let stats = rag::index_folder(&store, &client, &sample.to_string_lossy(), &cancel, &|_| {})
            .await
            .expect("failed to index the bundled sample through Lemonade");
        assert!(!stats.cancelled, "sample indexing was unexpectedly cancelled");
        assert!(
            store.stats().1 > 0,
            "sample indexing completed without producing chunks: {stats:?}"
        );
    }

    Some(LiveFixture {
        store,
        client,
        _guard: guard,
    })
}
