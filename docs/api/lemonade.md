- WS `/logs/stream` - Log Streaming (subscribe -> snapshot + live log entries)

## Log Streaming API (WebSocket) <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Stream server logs over WebSocket. Clients connect, send a subscribe message, and receive a snapshot of recent log history followed by live log entries as they occur.

### Connection

The WebSocket server shares the same port as the [Realtime Audio Transcription API](#realtime-audio-transcription-api-websocket). Discover the port via the [`/v1/health`](#get-apiv1health) endpoint (`websocket_port` field), then connect:

```
ws://localhost:<websocket_port>/logs/stream
```

After connecting, send a `logs.subscribe` message to start receiving logs.

### Client → Server Messages

| Message Type | Description |
|--------------|-------------|
| `logs.subscribe` | Subscribe to log stream. Optional `after_seq` field to resume from a specific sequence number. |

### Server → Client Messages

| Message Type | Description |
|--------------|-------------|
| `logs.snapshot` | Initial batch of retained log entries (up to 5000). Sent once after subscribing. |
| `logs.entry` | A single live log entry. Sent as new log lines are emitted. |
| `error` | Error message (e.g., invalid subscribe request). |

### Example: Subscribe to Logs

Subscribe from the beginning (full backlog):

```json
{
  "type": "logs.subscribe",
  "after_seq": null
}
```

Resume after a known sequence number (e.g., on reconnect):

```json
{
  "type": "logs.subscribe",
  "after_seq": 1042
}
```

### Example: Snapshot Response

```json
{
  "type": "logs.snapshot",
  "entries": [
    {
      "seq": 1,
      "timestamp": "2025-03-30 14:22:01.123",
      "severity": "Info",
      "tag": "Server",
      "line": "2025-03-30 14:22:01.123 [Info] (Server) Starting Lemonade Server..."
    }
  ]
}
```

### Example: Live Entry

```json
{
  "type": "logs.entry",
  "entry": {
    "seq": 1043,
    "timestamp": "2025-03-30 14:22:05.456",
    "severity": "Info",
    "tag": "Router",
    "line": "2025-03-30 14:22:05.456 [Info] (Router) Model loaded successfully"
  }
}
```

### Log Entry Fields

| Field | Type | Description |
|-------|------|-------------|
| `seq` | integer | Monotonically increasing sequence number. Use for dedup and resume. |
| `timestamp` | string | Formatted timestamp from the log system. |
| `severity` | string | Log level: `Trace`, `Debug`, `Info`, `Warning`, `Error`, `Fatal`. |
| `tag` | string | Log source tag (e.g., `Server`, `Router`, component name). |
| `line` | string | The full formatted log line. |

### Integration Notes

- **Reconnection**: Track the last `seq` received and pass it as `after_seq` on reconnect to avoid duplicate entries.
- **Backlog**: The server retains up to 5000 recent log entries. The snapshot may be smaller if fewer entries exist.
- **Platform availability**: WebSocket log streaming is available on all platforms (Windows, Linux, and macOS).
