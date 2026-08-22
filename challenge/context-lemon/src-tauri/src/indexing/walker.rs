use ignore::WalkBuilder;
use std::path::{Path, PathBuf};

const SKIP_DIRS: &[&str] = &[
    "node_modules",
    "target",
    ".git",
    "dist",
    "build",
    "__pycache__",
    ".venv",
    "venv",
    ".next",
    "vendor",
    ".idea",
    ".vscode",
    ".cargo-home",
];

const SUPPORTED_EXTENSIONS: &[&str] = &[
    "md", "markdown", "txt", "rst", "json", "yaml", "yml", "toml", "rs", "py", "js", "jsx", "ts",
    "tsx", "go", "java", "c", "h", "cpp", "hpp", "cs", "rb", "php", "html", "css", "scss", "sh",
    "ps1", "sql", "xml",
];

const MAX_FILE_BYTES: u64 = 5 * 1024 * 1024;

/// Would this path be indexed if the walker reached it? The file watcher uses this to
/// throw away the flood of events from directories we never index anyway — a single
/// `git status` in a watched repo would otherwise queue a re-index.
pub fn is_indexable_path(path: &Path) -> bool {
    if path
        .components()
        .any(|c| SKIP_DIRS.contains(&c.as_os_str().to_string_lossy().as_ref()))
    {
        return false;
    }
    path.extension()
        .and_then(|e| e.to_str())
        .map(|e| SUPPORTED_EXTENSIONS.contains(&e.to_lowercase().as_str()))
        .unwrap_or(false)
}

pub fn walk_folder(root: &Path) -> Vec<PathBuf> {
    let mut files = Vec::new();
    let walker = WalkBuilder::new(root)
        .hidden(true)
        .git_ignore(true)
        .git_global(true)
        .git_exclude(true)
        .filter_entry(|entry| {
            if entry.file_type().map(|ft| ft.is_dir()).unwrap_or(false) {
                let name = entry.file_name().to_string_lossy();
                return !SKIP_DIRS.iter().any(|skip| name.as_ref() == *skip);
            }
            true
        })
        .build();

    for result in walker {
        let entry = match result {
            Ok(e) => e,
            Err(_) => continue,
        };
        if !entry.file_type().map(|ft| ft.is_file()).unwrap_or(false) {
            continue;
        }
        let path = entry.path();
        let is_supported = path
            .extension()
            .and_then(|e| e.to_str())
            .map(|e| SUPPORTED_EXTENSIONS.contains(&e.to_lowercase().as_str()))
            .unwrap_or(false);
        if !is_supported {
            continue;
        }
        if let Ok(meta) = entry.metadata() {
            if meta.len() > MAX_FILE_BYTES {
                continue;
            }
        }
        files.push(path.to_path_buf());
    }
    files
}
