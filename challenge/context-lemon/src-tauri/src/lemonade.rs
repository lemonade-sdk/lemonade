use serde::Deserialize;
use serde_json::json;
use std::time::Duration;

pub struct LemonadeClient {
    base_url: String,
    http: reqwest::Client,
}

#[derive(Debug, Deserialize)]
struct EmbeddingsResponse {
    data: Vec<EmbeddingData>,
}

#[derive(Debug, Deserialize)]
struct EmbeddingData {
    embedding: Vec<f32>,
    #[serde(default)]
    index: Option<usize>,
}

/// Lemonade sometimes reports an application-level failure — e.g. a chunk that
/// tokenizes past the backend's batch-size limit — with an HTTP 200 and an
/// `{"error": {...}}` body instead of a non-2xx status. A 200 is therefore not proof
/// the body matches `EmbeddingsResponse`; this shape has to be checked for either way.
#[derive(Debug, Deserialize)]
struct ErrorEnvelope {
    error: ErrorDetail,
}

#[derive(Debug, Deserialize)]
struct ErrorDetail {
    message: String,
}

/// Marks an embed() error as "this specific input was rejected for being too large" —
/// as opposed to a connectivity/model/server failure — so callers can retry with a
/// smaller input instead of giving up. A plain string prefix (rather than a proper
/// error enum) matches the rest of this codebase's convention of `Result<_, String>`.
pub const EMBED_TOO_LARGE_PREFIX: &str = "EMBED_TOO_LARGE:";

/// Pulled out of `embed()` as a pure function so the exact response shapes Lemonade is
/// known to send — success, error-in-a-200, and a genuine non-2xx — can be tested
/// directly without spinning up an HTTP server.
fn parse_embeddings_response(
    body: &str,
    status: reqwest::StatusCode,
    expected_len: usize,
) -> Result<Vec<Vec<f32>>, String> {
    match serde_json::from_str::<EmbeddingsResponse>(body) {
        Ok(parsed) => {
            let mut ordered: Vec<Vec<f32>> = vec![Vec::new(); expected_len];
            for (pos, item) in parsed.data.into_iter().enumerate() {
                let idx = item.index.unwrap_or(pos);
                if idx < ordered.len() {
                    ordered[idx] = item.embedding;
                }
            }
            Ok(ordered)
        }
        Err(parse_err) => match serde_json::from_str::<ErrorEnvelope>(body) {
            Ok(envelope) if envelope.error.message.to_lowercase().contains("too large") => {
                Err(format!("{EMBED_TOO_LARGE_PREFIX}{}", envelope.error.message))
            }
            Ok(envelope) => Err(format!(
                "embeddings request failed ({status}): {}",
                envelope.error.message
            )),
            Err(_) if status.is_success() => Err(format!(
                "failed to parse embeddings response ({status}): {parse_err}"
            )),
            Err(_) => Err(format!("embeddings request returned {status}: {body}")),
        },
    }
}

#[derive(Debug, Deserialize)]
struct ChatResponse {
    choices: Vec<ChatChoice>,
}

#[derive(Debug, Deserialize)]
struct ChatChoice {
    message: ChatMessage,
}

#[derive(Debug, Deserialize)]
struct ChatMessage {
    content: Option<String>,
}

impl LemonadeClient {
    pub fn new(base_url: String) -> Self {
        let http = reqwest::Client::builder()
            .timeout(Duration::from_secs(120))
            .build()
            .expect("failed to build reqwest client");
        LemonadeClient { base_url, http }
    }

    pub async fn embed(&self, texts: &[String], model: &str) -> Result<Vec<Vec<f32>>, String> {
        if texts.is_empty() {
            return Ok(Vec::new());
        }
        let url = format!("{}/embeddings", self.base_url);

        // A transient send failure (observed in practice: an isolated dropped loopback
        // connection during a long run, with the server otherwise healthy throughout)
        // must not abort an entire folder's indexing job. Retried a couple of times
        // with a short backoff before surfacing it as a real failure; a genuinely
        // unreachable or crashed server still fails after these attempts.
        const MAX_SEND_ATTEMPTS: u32 = 3;
        let mut resp = None;
        let mut send_err = String::new();
        for attempt in 1..=MAX_SEND_ATTEMPTS {
            match self
                .http
                .post(&url)
                .header("Authorization", "Bearer lemonade")
                .json(&json!({ "model": model, "input": texts }))
                .send()
                .await
            {
                Ok(r) => {
                    resp = Some(r);
                    break;
                }
                Err(e) => {
                    send_err = format!("embeddings request failed: {e}");
                    if attempt < MAX_SEND_ATTEMPTS {
                        tokio::time::sleep(Duration::from_millis(300 * attempt as u64)).await;
                    }
                }
            }
        }
        let resp = resp.ok_or(send_err)?;

        let status = resp.status();
        let body = resp
            .text()
            .await
            .map_err(|e| format!("failed to read embeddings response: {e}"))?;

        parse_embeddings_response(&body, status, texts.len())
    }

    pub async fn chat(&self, model: &str, system: &str, user: &str, no_think: bool) -> Result<String, String> {
        let url = format!("{}/chat/completions", self.base_url);
        let user_content = if no_think {
            format!("{user}\n/no_think")
        } else {
            user.to_string()
        };

        let resp = self
            .http
            .post(&url)
            .header("Authorization", "Bearer lemonade")
            .json(&json!({
                "model": model,
                "stream": false,
                "messages": [
                    { "role": "system", "content": system },
                    { "role": "user", "content": user_content },
                ],
            }))
            .send()
            .await
            .map_err(|e| format!("chat request failed: {e}"))?;

        if !resp.status().is_success() {
            let status = resp.status();
            let body = resp.text().await.unwrap_or_default();
            return Err(format!("chat request returned {status}: {body}"));
        }

        let parsed: ChatResponse = resp
            .json()
            .await
            .map_err(|e| format!("failed to parse chat response: {e}"))?;

        let content = parsed
            .choices
            .into_iter()
            .next()
            .and_then(|c| c.message.content)
            .unwrap_or_default();

        let answer = strip_thinking(&content);
        if answer.is_empty() {
            // Better an explicit error than a blank answer box sitting under five
            // citations, which reads as a broken app rather than a failed request.
            return Err("the model returned an empty response".to_string());
        }
        Ok(answer)
    }

    pub async fn is_reachable(&self) -> bool {
        let url = format!("{}/models", self.base_url);
        self.http
            .get(&url)
            .header("Authorization", "Bearer lemonade")
            .send()
            .await
            .map(|r| r.status().is_success())
            .unwrap_or(false)
    }
}

/// Qwen3 wraps its reasoning in `<think>…</think>`. Returns the visible answer with
/// every such block removed, alongside the reasoning that was removed.
///
/// Taking everything after the *last* `</think>` — the obvious implementation — returns
/// an empty string whenever the model keeps its whole reply inside the block or the
/// response is truncated mid-reasoning, which surfaces as a blank answer.
fn split_thinking(content: &str) -> (String, String) {
    const OPEN: &str = "<think>";
    const CLOSE: &str = "</think>";

    let mut visible = String::new();
    let mut thought = String::new();
    let mut rest = content;

    loop {
        let Some(start) = rest.find(OPEN) else {
            visible.push_str(rest);
            break;
        };
        visible.push_str(&rest[..start]);
        let after = &rest[start + OPEN.len()..];
        match after.find(CLOSE) {
            Some(end) => {
                thought.push_str(&after[..end]);
                rest = &after[end + CLOSE.len()..];
            }
            None => {
                // Unclosed block: the reply was cut off while still reasoning.
                thought.push_str(after);
                break;
            }
        }
    }

    (visible.trim().to_string(), thought.trim().to_string())
}

/// The visible answer, falling back to the reasoning text when the model left nothing
/// outside the block. Showing its reasoning is imperfect; showing nothing is worse.
fn strip_thinking(content: &str) -> String {
    let (visible, thought) = split_thinking(content);
    if visible.is_empty() {
        thought
    } else {
        visible
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strips_a_normal_thinking_block() {
        assert_eq!(
            strip_thinking("<think>weighing options</think>\n\nPort 7913."),
            "Port 7913."
        );
    }

    #[test]
    fn empty_think_block_from_no_think_leaves_the_answer() {
        assert_eq!(strip_thinking("<think>\n\n</think>\n\nPort 7913."), "Port 7913.");
    }

    #[test]
    fn plain_content_is_untouched() {
        assert_eq!(strip_thinking("Port 7913."), "Port 7913.");
    }

    /// The regression that produced a blank answer box under five citations.
    #[test]
    fn answer_left_entirely_inside_the_block_is_not_lost() {
        assert_eq!(
            strip_thinking("<think>The context does not mention a budget.</think>"),
            "The context does not mention a budget."
        );
    }

    /// A reply cut off mid-reasoning still yields something rather than nothing.
    #[test]
    fn unclosed_block_falls_back_to_the_reasoning() {
        assert_eq!(
            strip_thinking("<think>The context does not mention it"),
            "The context does not mention it"
        );
    }

    #[test]
    fn text_around_multiple_blocks_is_preserved_in_order() {
        assert_eq!(
            strip_thinking("A<think>x</think>B<think>y</think>C"),
            "ABC"
        );
    }

    #[test]
    fn genuinely_empty_content_stays_empty_so_the_caller_can_error() {
        assert_eq!(strip_thinking(""), "");
        assert_eq!(strip_thinking("<think></think>"), "");
    }

    #[test]
    fn parses_a_normal_successful_response() {
        let body = r#"{"data":[{"embedding":[0.1,0.2],"index":0,"object":"embedding"}],"model":"m","object":"list","usage":{}}"#;
        let result = parse_embeddings_response(body, reqwest::StatusCode::OK, 1).unwrap();
        assert_eq!(result, vec![vec![0.1, 0.2]]);
    }

    /// The exact regression: Lemonade rejected a 2093-char / 888-token config file
    /// with an HTTP 200 and this body, which `resp.json::<EmbeddingsResponse>()`
    /// used to fail on with an opaque "error decoding response body" — no indication
    /// anywhere that the real problem was chunk size, not a malformed response.
    #[test]
    fn error_wrapped_in_http_200_is_detected_and_labelled_too_large() {
        let body = r#"{"error":{"code":500,"details":{"backend":"llama-server","response":{"error":{"code":500,"message":"input (888 tokens) is too large to process. increase the physical batch size (current batch size: 512)","type":"server_error"}}},"message":"input (888 tokens) is too large to process. increase the physical batch size (current batch size: 512)","status_code":500,"type":"server_error"}}"#;
        let err = parse_embeddings_response(body, reqwest::StatusCode::OK, 1).unwrap_err();
        assert!(
            err.starts_with(EMBED_TOO_LARGE_PREFIX),
            "expected the too-large marker, got: {err}"
        );
        assert!(err.contains("888 tokens"), "expected the real message preserved, got: {err}");
    }

    #[test]
    fn error_in_200_that_is_not_a_size_problem_is_not_mislabelled() {
        let body = r#"{"error":{"message":"model not found","code":404}}"#;
        let err = parse_embeddings_response(body, reqwest::StatusCode::OK, 1).unwrap_err();
        assert!(!err.starts_with(EMBED_TOO_LARGE_PREFIX));
        assert!(err.contains("model not found"));
    }

    #[test]
    fn genuine_non_2xx_without_json_body_still_reports_the_raw_body() {
        let err = parse_embeddings_response(
            "internal server error",
            reqwest::StatusCode::INTERNAL_SERVER_ERROR,
            1,
        )
        .unwrap_err();
        assert!(err.contains("internal server error"));
        assert!(!err.starts_with(EMBED_TOO_LARGE_PREFIX));
    }
}
