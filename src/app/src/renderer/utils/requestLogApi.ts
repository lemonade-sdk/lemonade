import { serverConfig } from './serverConfig';

export interface RequestLogEntry {
  id: number;
  created_at: string;
  client_ip: string | null;
  forwarded_for: string | null;
  method: string;
  path: string;
  query_string: string | null;
  status_code: number | null;
  duration_ms: number | null;
  user_agent: string | null;
  endpoint_type: string | null;
  model: string | null;
  keep_alive: string | null;
  stream: boolean | null;
  request_body_bytes: number | null;
  response_body_bytes: number | null;
  prompt_chars: number | null;
  messages_chars: number | null;
  redacted_body: unknown;
  error: string | null;
}

export interface RequestLogSearchResponse {
  entries: RequestLogEntry[];
  limit?: number;
  offset?: number;
}

export interface RequestLogStats {
  since: string;
  total_requests: number;
  avg_duration_ms: number;
  unique_client_ips: number;
  keep_alive_requests: number;
  by_endpoint_type: Record<string, number>;
  by_model: Record<string, number>;
}

export interface RequestLogSearchParams {
  model?: string;
  client_ip?: string;
  path?: string;
  since?: string;
  keep_alive?: string;
  limit?: number;
  offset?: number;
}

export class RequestLogApiError extends Error {
  status: number;

  constructor(message: string, status: number) {
    super(message);
    this.name = 'RequestLogApiError';
    this.status = status;
  }
}

function buildQuery(params: Record<string, string | number | undefined>): string {
  const search = new URLSearchParams();
  for (const [key, value] of Object.entries(params)) {
    if (value !== undefined && value !== '') {
      search.set(key, String(value));
    }
  }
  const query = search.toString();
  return query ? `?${query}` : '';
}

async function parseJsonResponse<T>(response: Response): Promise<T> {
  const readErrorMessage = async (fallback: string): Promise<string> => {
    try {
      const body = await response.json();
      if (body?.error && typeof body.error === 'string') {
        return body.error;
      }
    } catch {
      // ignore parse errors
    }
    return fallback;
  };

  if (response.status === 401) {
    throw new RequestLogApiError(
      await readErrorMessage('API key required — set it in Settings → Connection'),
      401,
    );
  }
  if (response.status === 503) {
    throw new RequestLogApiError(
      await readErrorMessage(
        'Request logging is not enabled or the database is unavailable',
      ),
      503,
    );
  }
  if (!response.ok) {
    throw new RequestLogApiError(
      await readErrorMessage(`Request failed (${response.status})`),
      response.status,
    );
  }
  return response.json() as Promise<T>;
}

export async function fetchRequestLogSearch(
  params: RequestLogSearchParams,
): Promise<RequestLogSearchResponse> {
  const response = await serverConfig.fetch(
    `/request-log/search${buildQuery({
      model: params.model,
      client_ip: params.client_ip,
      path: params.path,
      since: params.since,
      keep_alive: params.keep_alive,
      limit: params.limit ?? 100,
      offset: params.offset ?? 0,
    })}`,
  );
  return parseJsonResponse<RequestLogSearchResponse>(response);
}

export async function fetchRequestLogStats(since = '24h'): Promise<RequestLogStats> {
  const response = await serverConfig.fetch(
    `/request-log/stats${buildQuery({ since })}`,
  );
  return parseJsonResponse<RequestLogStats>(response);
}

export async function fetchRequestLogRecent(limit = 100): Promise<RequestLogSearchResponse> {
  const response = await serverConfig.fetch(
    `/request-log/recent${buildQuery({ limit })}`,
  );
  return parseJsonResponse<RequestLogSearchResponse>(response);
}
