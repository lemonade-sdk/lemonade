# Context-Lemon 90-second demo script

This script is designed for one continuous recording. Keep the desktop resolution at
1920×1080, increase the app window to roughly 900×800, and hide personal paths or
notifications before recording.

## Prepare once

1. Start Lemonade Server and verify that `nomic-embed-text-v1-GGUF` and
   `Qwen3-0.6B-GGUF` are available.
2. Copy `sample/` to a disposable folder outside the repository. Use the copy during
   the demo so the source corpus remains unchanged.
3. Launch Context-Lemon and add the disposable folder.
4. Wait for indexing to finish, then confirm the port question returns `7913` with
   `faq.md` in the citation list.
5. Reset the app to the opening state you want to record.

## Shot list and narration

| Time | On screen | Suggested narration |
| --- | --- | --- |
| 0–8 s | Lemonade Server running, then Context-Lemon | “Context-Lemon gives AMD Lemonade private memory over local files.” |
| 8–20 s | Add the disposable sample folder | “I point it at a folder. Files are chunked and embedded locally through Lemonade.” |
| 20–30 s | Index status reaches four files | “The index is incremental, gitignore-aware, disk-backed, and continuously watched.” |
| 30–48 s | Ask: “What port does the Nightingale gateway listen on by default?” | “The answer is generated from the retrieved context, not a cloud service.” |
| 48–58 s | Hold on the answer and citation list | “Every result includes the exact files and line ranges used. The citation is the product.” |
| 58–70 s | Open the disposable `faq.md` and change `7913` to `7921` | “When a source changes, the watcher automatically re-indexes only what changed.” |
| 70–82 s | Return to Context-Lemon and wait for the updated index message | “No manual rebuild and no document upload are required.” |
| 82–90 s | Ask the port question again; show `7921` and the citation | “The updated answer is grounded in the updated local file—fully offline.” |

## Capture these screenshots after the video

Save images under `docs/media/` so the README can link to stable relative paths.

1. `01-folder-indexed.png` — full app with Lemonade connected, the folder visible,
   and index counts readable.
2. `02-grounded-answer.png` — port answer plus the complete file-and-line citation
   list.
3. `03-live-reindex.png` — the automatic indexing status immediately after the file
   edit, or a clear updated answer if the transient status is too fast to capture.

Crop only empty desktop space. Do not crop away the Lemonade connection state, index
counts, question, answer, or citations. Use descriptive alt text when adding the
images to the README.

## Recording notes

- Target 60–120 seconds; 90 seconds is ideal.
- Record at 1080p and export H.264 MP4.
- Keep narration factual and visible on screen—avoid unverified performance claims.
- Put the final YouTube or unlisted video link directly below the README pitch.
- Restore the disposable corpus after recording; do not edit the bundled sample used
  by tests.
