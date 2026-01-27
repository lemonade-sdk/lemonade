# Lemonade Server Prometheus Exporter

Prometheus exporter for monitoring Lemonade Server runtime metrics. This
program crapes metrics from Lemonade Server's HTTP API endpoints and (modified)
llama.cpp backend metrics endpoints.

## Features

- Scrapes `/api/v1/stats` for performance metrics (tokens/sec, TTFT, token
  counts)
- Scrapes `/api/v1/health` for server status and model information
- Tracks per-token decode timing via `decode_token_times` from streaming
  proxy
- Estimates concurrent users and active sessions from request patterns
- Calculates cache hit rates from prompt and input token differences
- Scrapes llama.cpp backend `/metrics` endpoints for detailed backend
  performance
- Exposes all metrics in Prometheus text format on `/metrics`

# Local Installation

```bash
# Or install from project requirements
pip3 install -r ../requirements.txt
```

## Usage

```bash
python3 prometheus/lemonade-exporter.py
```

This will:
- Connect to Lemonade Server at `http://localhost:8000`
- Listen on port `9091`
- Expose metrics at `http://localhost:9091/metrics`

# System-Wide Installation (Running as a Systemd Service)

Copy the provided systemd service file to `/etc/systemd/system` and then enable
and start the service:

```bash
sudo systemctl enable lemonade-exporter.service
sudo systemctl start lemonade-exporter.service
sudo systemctl status lemonade-exporter.service
```

# Prometheus Configuration

Add the exporter to your `prometheus.yml`. Note that if your Prometheus server
is running on a different node you will want to use the IP address of your
lemonade-serve node instead of `localhost`.

```yaml
scrape_configs:
  # ... existing configs ...
  
  # Lemonade Server Exporter
  - job_name: 'lemonade'
    scrape_interval: 5s
    scrape_timeout: 5s
    static_configs:
      - targets: ['localhost:9091']
```

# Example Grafana Dashboard

An example [Grafana dashboard](./grafana/grafana-dashboard.json) is included
for visualizing Lemonade Server metrics along with CPU and AMD GPU metrics
from node-exporter and AMD GPU metrics exporter.

