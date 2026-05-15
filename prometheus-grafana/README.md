# Lemonade Prometheus and Grafana

This directory contains a minimal Prometheus/Grafana setup for Lemonade Server's built-in `GET /metrics` endpoint.

## Refresh Rate

Lemonade does not push metrics and `/metrics` has no internal refresh timer. The endpoint renders current state whenever it is scraped.

Prometheus controls how often Lemonade is polled. Configure this with `scrape_interval` in `prometheus.yml`:

```yaml
global:
  scrape_interval: 10s
```

Grafana controls how often dashboard panels query Prometheus. Grafana does not control how often Prometheus scrapes Lemonade.

## Docker Compose DNS

Docker Compose services can reach each other by service name on the Compose network.

Grafana should use this Prometheus data source URL:

```text
http://prometheus:9090
```

For the localhost test config, Prometheus scrapes Lemonade on the host machine at:

```text
http://host.docker.internal:13305/metrics
```

The request path is:

```text
Grafana -> http://prometheus:9090 -> http://host.docker.internal:13305/metrics
```

## Default Compose Stack

Use `docker-compose.yml` when Lemonade is also running as a service in the same Compose project.

Prometheus target:

```yaml
scrape_configs:
  - job_name: 'lemonade'
    metrics_path: /metrics
    static_configs:
      - targets: ['lemonade:13305']
```

In this mode, the service DNS name is:

```text
lemonade
```

## Localhost Test Stack

Use `docker-compose.localhost.yml` when Lemonade is running directly on the host at port `13305` and only Prometheus/Grafana are running in Docker.

Start Prometheus:

```bash
docker compose -f docker-compose.localhost.yml up -d prometheus
```

Start Grafana too:

```bash
docker compose -f docker-compose.localhost.yml up -d
```

Prometheus target:

```yaml
scrape_configs:
  - job_name: 'lemonade-localhost'
    metrics_path: /metrics
    static_configs:
      - targets: ['host.docker.internal:13305']
```

The Compose file includes Linux host gateway support:

```yaml
extra_hosts:
  - "host.docker.internal:host-gateway"
```

Stop the test stack:

```bash
docker compose -f docker-compose.localhost.yml down
```

## Lemonade Metrics Endpoint

Lemonade exposes metrics at:

```text
GET /metrics
HEAD /metrics
```

The endpoint is root-level only. It is not available under `/api/v1`, `/v1`, `/api/v0`, or `/v0`.

If `LEMONADE_API_KEY` is set, Prometheus must send a bearer token. Either `LEMONADE_API_KEY` or `LEMONADE_ADMIN_API_KEY` is accepted.

Example Prometheus config with an API key:

```yaml
scrape_configs:
  - job_name: 'lemonade'
    metrics_path: /metrics
    authorization:
      type: Bearer
      credentials: your-token-here
    static_configs:
      - targets: ['host.docker.internal:13305']
```

## Main Metric Families

- `lemonade_server_up`
- `lemonade_server_info`
- `lemonade_loaded_models`
- `lemonade_model_info`
- `lemonade_max_loaded_models`
- `lemonade_model_*` latest per-model telemetry gauges
- `lemonade_model_decode_token_time_*` latest streaming token interval gauges
- `lemonade_model_*_total` per-model counters
- `lemonade_*_total` aggregate counters
- `lemonade_cpu_usage_percent`
- `lemonade_memory_used_gb`
- `lemonade_gpu_usage_percent`
- `lemonade_vram_used_gb`
- `lemonade_npu_usage_percent`
- `lemonade_llamacpp_*` best-effort normalized llama.cpp backend metrics

The built-in dashboard also includes optional panels for AMD GPU exporter metrics inspired by the RFC dashboard in PR #996:

- `amd_gpu_edge_temperature`
- `amd_gpu_junction_temperature`
- `amd_gpu_memory_temperature`
- `amd_gpu_average_package_power`
- `amd_gpu_used_vram`
- `amd_gpu_total_vram`

Those panels are empty unless Prometheus is also scraping an exporter that provides those metric families.

See `docs/api/lemonade.md` for the full endpoint contract.
