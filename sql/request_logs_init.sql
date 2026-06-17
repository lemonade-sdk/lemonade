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
  redacted_body JSONB,
  error TEXT
);

CREATE INDEX IF NOT EXISTS idx_request_logs_created_at ON request_logs (created_at);
CREATE INDEX IF NOT EXISTS idx_request_logs_model ON request_logs (model);
CREATE INDEX IF NOT EXISTS idx_request_logs_client_ip ON request_logs (client_ip);
CREATE INDEX IF NOT EXISTS idx_request_logs_path ON request_logs (path);
CREATE INDEX IF NOT EXISTS idx_request_logs_keep_alive ON request_logs (keep_alive);
