// Lemonade's embedding backend rejects any single input over 512 tokens. Plain
// English prose runs roughly 4 chars/token, so 2000 chars sat right at that ceiling
// with no margin; punctuation- or escape-dense text (JSON, minified code) can run
// under 2.5 chars/token, well past it. 1400 leaves real headroom for ordinary text;
// rag.rs's resilient embed-and-split retry is the actual backstop for anything denser.
const TARGET_CHARS: usize = 1400;
const OVERLAP_CHARS: usize = 250;
const MAX_LINE_CHUNK_CHARS: usize = 4000;

#[derive(Debug, Clone)]
pub struct ChunkSpan {
    pub text: String,
    pub start_line: usize,
    pub end_line: usize,
}

struct Piece<'a> {
    text: &'a str,
    line_no: usize,
}

fn split_long_line<'a>(line: &'a str, line_no: usize, out: &mut Vec<Piece<'a>>) {
    if line.len() <= MAX_LINE_CHUNK_CHARS {
        out.push(Piece { text: line, line_no });
        return;
    }
    let mut start = 0usize;
    while start < line.len() {
        let mut end = (start + MAX_LINE_CHUNK_CHARS).min(line.len());
        while end < line.len() && !line.is_char_boundary(end) {
            end += 1;
        }
        out.push(Piece {
            text: &line[start..end],
            line_no,
        });
        start = end;
    }
}

pub fn chunk_text(text: &str) -> Vec<ChunkSpan> {
    let mut chunks = Vec::new();
    if text.trim().is_empty() {
        return chunks;
    }

    let mut pieces: Vec<Piece> = Vec::new();
    for (idx, raw_line) in text.split_inclusive('\n').enumerate() {
        split_long_line(raw_line, idx + 1, &mut pieces);
    }
    if pieces.is_empty() {
        return chunks;
    }

    let mut i = 0usize;
    while i < pieces.len() {
        let mut buf = String::new();
        let mut j = i;
        while j < pieces.len() && buf.len() < TARGET_CHARS {
            buf.push_str(pieces[j].text);
            j += 1;
        }

        let trimmed = buf.trim_end();
        if !trimmed.is_empty() {
            chunks.push(ChunkSpan {
                text: trimmed.to_string(),
                start_line: pieces[i].line_no,
                end_line: pieces[j - 1].line_no,
            });
        }

        if j >= pieces.len() {
            break;
        }

        let mut back = j;
        let mut overlap_len = 0usize;
        while back > i + 1 && overlap_len < OVERLAP_CHARS {
            back -= 1;
            overlap_len += pieces[back].text.len();
        }
        i = back.max(i + 1);
    }

    chunks
}
