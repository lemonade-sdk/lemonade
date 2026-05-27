// api.js — Connection layer for the lemonade UI redesign prototype.
//
// Wraps all fetch calls to the lemond HTTP server.
// No framework, no build step. Exposes `window.LemonadeAPI`.
//
// Usage (from app.js):
//   const api = window.LemonadeAPI;
//   api.onStatusChange(status => { ... });
//   await api.connect();

(() => {
  'use strict';

  const DEFAULT_BASE_URL = 'http://localhost:13305';
  const LS_BASE_URL = 'lemonade_base_url';
  const LS_API_KEY = 'lemonade_api_key';

  const DISCONNECTED = 'disconnected';
  const CONNECTING = 'connecting';
  const CONNECTED = 'connected';

  class API {
    constructor() {
      this._status = DISCONNECTED;
      this._listeners = [];
      this._healthData = null;
      this._modelsData = null;
      this._systemInfoData = null;
      this._pollTimer = null;
    }

    // ── Config (persisted in localStorage) ──────────────────────────

    get baseUrl() {
      return (localStorage.getItem(LS_BASE_URL) || DEFAULT_BASE_URL).replace(/\/+$/, '');
    }

    set baseUrl(url) {
      localStorage.setItem(LS_BASE_URL, url.replace(/\/+$/, ''));
    }

    get apiKey() {
      return localStorage.getItem(LS_API_KEY) || '';
    }

    set apiKey(key) {
      if (key) localStorage.setItem(LS_API_KEY, key);
      else localStorage.removeItem(LS_API_KEY);
    }

    // ── Connection status ───────────────────────────────────────────

    get status() { return this._status; }
    get isConnected() { return this._status === CONNECTED; }
    get healthData() { return this._healthData; }
    get modelsData() { return this._modelsData; }
    get systemInfoData() { return this._systemInfoData; }

    /** Returns loaded models array from last health check. */
    get loadedModels() {
      return this._healthData?.all_models_loaded || [];
    }

    /** Returns all models array from last models fetch. */
    get allModels() {
      return this._modelsData?.data || [];
    }

    onStatusChange(fn) {
      this._listeners.push(fn);
      return () => { this._listeners = this._listeners.filter(f => f !== fn); };
    }

    _setStatus(s) {
      if (this._status === s) return;
      this._status = s;
      this._listeners.forEach(fn => { try { fn(s); } catch {} });
    }

    // ── Fetch wrapper ───────────────────────────────────────────────

    _headers(extra) {
      const h = { ...(extra || {}) };
      if (this.apiKey) h['Authorization'] = `Bearer ${this.apiKey}`;
      return h;
    }

    async _fetch(path, opts = {}) {
      const url = `${this.baseUrl}${path}`;
      const headers = this._headers(opts.headers);

      if (opts.body && typeof opts.body === 'object' &&
          !(opts.body instanceof FormData) &&
          !(opts.body instanceof ReadableStream)) {
        headers['Content-Type'] = 'application/json';
        opts = { ...opts, body: JSON.stringify(opts.body) };
      }

      const resp = await fetch(url, { ...opts, headers });
      if (!resp.ok) {
        const text = await resp.text().catch(() => '');
        const err = new Error(text || resp.statusText || `HTTP ${resp.status}`);
        err.status = resp.status;
        throw err;
      }
      return resp;
    }

    async _json(path, opts) {
      const resp = await this._fetch(path, opts);
      return resp.json();
    }

    // ── Endpoints ───────────────────────────────────────────────────

    async health() {
      const data = await this._json('/api/v1/health');
      this._healthData = data;
      this._setStatus(CONNECTED);
      return data;
    }

    async models(showAll = true) {
      const qs = showAll ? '?show_all=true' : '';
      const data = await this._json(`/api/v1/models${qs}`);
      this._modelsData = data;
      return data;
    }

    async modelDetail(id) {
      return this._json(`/api/v1/models/${encodeURIComponent(id)}`);
    }

    async loadModel(modelName, recipeOptions) {
      const body = { model_name: modelName };
      if (recipeOptions) body.recipe_options = recipeOptions;
      return this._json('/api/v1/load', { method: 'POST', body });
    }

    async unloadModel(modelName) {
      const body = modelName ? { model_name: modelName } : {};
      return this._json('/api/v1/unload', { method: 'POST', body });
    }

    async systemInfo() {
      const data = await this._json('/api/v1/system-info');
      this._systemInfoData = data;
      return data;
    }

    // ── SSE: Pull (model download) ──────────────────────────────────

    async pullModel(modelName, { onProgress, onComplete, onError } = {}) {
      try {
        const resp = await this._fetch('/api/v1/pull', {
          method: 'POST',
          body: { model: modelName, stream: true },
        });
        const reader = resp.body.getReader();
        const dec = new TextDecoder();
        let buf = '';

        while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          buf += dec.decode(value, { stream: true });
          const lines = buf.split('\n');
          buf = lines.pop();

          for (const line of lines) {
            if (line.startsWith('event:')) continue;
            if (!line.startsWith('data: ')) continue;
            try {
              const d = JSON.parse(line.slice(6));
              if (d.percent !== undefined) onProgress?.(d);
              else onComplete?.(d);
            } catch {}
          }
        }
        onComplete?.({});
      } catch (err) {
        onError?.(err);
      }
    }

    // ── SSE: Chat completions ───────────────────────────────────────

    async chatCompletion(model, messages, { onToken, onReasoning, onDone, onError, params } = {}) {
      const t0 = performance.now();
      let firstTokenTime = null;
      let tokenCount = 0;
      let reasoningTokenCount = 0;

      try {
        const body = { model, messages, stream: true, ...(params || {}) };
        const resp = await this._fetch('/api/v1/chat/completions', {
          method: 'POST',
          body,
        });

        const reader = resp.body.getReader();
        const dec = new TextDecoder();
        let buf = '';
        let full = '';
        let reasoning = '';
        let respId = null;

        while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          buf += dec.decode(value, { stream: true });
          const lines = buf.split('\n');
          buf = lines.pop();

          for (const line of lines) {
            const t = line.trim();
            if (!t || !t.startsWith('data: ')) continue;
            const payload = t.slice(6);
            if (payload === '[DONE]') {
              const elapsed = performance.now() - t0;
              const totalTokens = tokenCount + reasoningTokenCount;
              const tps = totalTokens > 0 ? (totalTokens / (elapsed / 1000)).toFixed(1) : '0';
              const ttft = firstTokenTime ? (firstTokenTime - t0).toFixed(0) : null;
              onDone?.({ content: full, reasoning, id: respId, tps, ttft, tokens: tokenCount, reasoningTokens: reasoningTokenCount });
              return;
            }
            try {
              const chunk = JSON.parse(payload);
              // Detect server-side error in SSE stream
              if (chunk.error) {
                onError?.(new Error(chunk.error.message || 'Server streaming error'));
                return;
              }
              respId = chunk.id || respId;
              const delta = chunk.choices?.[0]?.delta;
              // Handle reasoning/thinking tokens (Qwen3.5, etc.)
              if (delta?.reasoning_content) {
                if (!firstTokenTime) firstTokenTime = performance.now();
                reasoningTokenCount++;
                reasoning += delta.reasoning_content;
                onReasoning?.(delta.reasoning_content, reasoning);
              }
              if (delta?.content) {
                if (!firstTokenTime) firstTokenTime = performance.now();
                tokenCount++;
                full += delta.content;
                onToken?.(delta.content, full);
              }
            } catch {}
          }
        }
        // Stream ended without [DONE]
        const elapsed = performance.now() - t0;
        const totalTokens = tokenCount + reasoningTokenCount;
        const tps = totalTokens > 0 ? (totalTokens / (elapsed / 1000)).toFixed(1) : '0';
        const ttft = firstTokenTime ? (firstTokenTime - t0).toFixed(0) : null;
        onDone?.({ content: full, reasoning, id: respId, tps, ttft, tokens: tokenCount, reasoningTokens: reasoningTokenCount });
      } catch (err) {
        onError?.(err);
      }
    }

    // ── Connection management ───────────────────────────────────────

    async connect() {
      this._setStatus(CONNECTING);
      try {
        await this.health();
        return true;
      } catch {
        this._setStatus(DISCONNECTED);
        this._healthData = null;
        return false;
      }
    }

    /** Fetch health + models in one call. Returns null on failure. */
    async refresh() {
      try {
        const [health, models] = await Promise.all([
          this.health(),
          this.models(true),
        ]);
        return { health, models };
      } catch {
        this._setStatus(DISCONNECTED);
        this._healthData = null;
        return null;
      }
    }

    startPolling(ms = 15000) {
      this.stopPolling();
      this._pollTimer = setInterval(() => this.connect(), ms);
    }

    stopPolling() {
      if (this._pollTimer) {
        clearInterval(this._pollTimer);
        this._pollTimer = null;
      }
    }
  }

  // Expose singleton
  window.LemonadeAPI = new API();

  // Re-export constants for convenience
  window.LemonadeAPI.DISCONNECTED = DISCONNECTED;
  window.LemonadeAPI.CONNECTING = CONNECTING;
  window.LemonadeAPI.CONNECTED = CONNECTED;
})();
