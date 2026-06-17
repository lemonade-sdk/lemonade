-- Lemonade HTTP request log schema (idempotent).
CREATE TABLE IF NOT EXISTS request_logs (
  id BIGSERIAL PRIMARY KEY,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  client_ip TEXT,
  forwarded_for TEXT,
  method TEXT NOT NULL,
  path TEXT NOT NULL,
  query_string TEXT,
  status_code INTEGER,
  duration_ms INTEGER,
  user_agent TEXT,
  endpoint_type TEXT,
  model TEXT,
  keep_alive TEXT,
  stream BOOLEAN,
  request_body_bytes INTEGER,
  response_body_bytes INTEGER,
  prompt_chars INTEGER,
  messages_chars INTEGER,
  prompt_tokens INTEGER,
  completion_tokens INTEGER,
  redacted_body JSONB,
  redacted_response JSONB,
  error TEXT
);

CREATE INDEX IF NOT EXISTS idx_request_logs_created_at ON request_logs (created_at);
CREATE INDEX IF NOT EXISTS idx_request_logs_model ON request_logs (model);
CREATE INDEX IF NOT EXISTS idx_request_logs_client_ip ON request_logs (client_ip);
CREATE INDEX IF NOT EXISTS idx_request_logs_path ON request_logs (path);
CREATE INDEX IF NOT EXISTS idx_request_logs_keep_alive ON request_logs (keep_alive);

ALTER TABLE request_logs ADD COLUMN IF NOT EXISTS prompt_tokens INTEGER;
ALTER TABLE request_logs ADD COLUMN IF NOT EXISTS completion_tokens INTEGER;
ALTER TABLE request_logs ADD COLUMN IF NOT EXISTS redacted_response JSONB;
