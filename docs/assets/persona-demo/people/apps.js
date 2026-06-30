// People · section 1 · "Connect to apps"
// Slides: the featured-apps board, connecting any OpenAI-compatible app, and
// adding Lemonade as an MCP server.
(function () {
  var P = window.LemonadePersona;
  if (!P) return;
  var h = P.helpers;
  var escapeText = h.escapeText;

  P.registerSection('people', 1, {
    title: 'Connect to apps',
    slides: [
      {
        label: 'Featured apps',
        demo: 'apps-board',
        caption: 'From coding agents to productivity tools — connect the apps you already love, with no API costs.',
        captionHref: 'https://lemonade-server.ai/marketplace.html',
        animationMode: 'once'
      },
      {
        label: 'Connect any OpenAI app',
        demo: 'apps-connect',
        caption: 'Point any OpenAI-compatible app at Lemonade — just set the base URL and connect.',
        captionHref: 'https://lemonade-server.ai/docs/api/openai/',
        animationMode: 'once'
      },
      {
        label: 'Add as an MCP server',
        demo: 'apps-mcp',
        caption: 'Expose your local models as MCP tools any compatible client can call.',
        captionHref: 'https://lemonade-server.ai/docs/api/mcp/',
        animationMode: 'once'
      }
    ]
  });

  // Curated snapshot of the Lemonade Marketplace (apps.json); ids are the logo
  // folder slugs. The three non-self-host groups feed the board below.
  var APP_LOGO = 'https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/';
  var appCategories = {
    'apps-coding': [
      { id: 'claude-code', name: 'Claude Code', desc: 'Agentic coding tool that reads your codebase, edits files, and runs commands.' },
      { id: 'github-copilot', name: 'GitHub Copilot', desc: 'VS Code Copilot extension for local AI coding assistance.' },
      { id: 'pi', name: 'Pi', desc: 'Minimal terminal-based coding agent using local models via Lemonade.' }
    ],
    'apps-personal': [
      { id: 'anythingllm', name: 'AnythingLLM', desc: 'All-in-one AI application for productivity using on-device models.' },
      { id: 'gaia', name: 'GAIA', desc: 'Python SDK for designing multi-modal local-first agents.' },
      { id: 'fx-chatbot', name: 'Firefox Chatbot', desc: 'Run Lemonade inside your Firefox browser.' }
    ],
    'apps-productivity': [
      { id: 'n8n', name: 'n8n', desc: 'Workflow automation with native Lemonade integration for AI-powered automations.' },
      { id: 'morphik', name: 'Morphik', desc: 'Centralize business knowledge and build reliable AI agents to automate tasks.' },
      { id: 'dify', name: 'Dify', desc: 'Build node-based AI agents and RAG workflows.' }
    ]
  };

  // "Connect to apps" overview: the nine featured apps on a single LIGHT stage (no
  // window chrome), grouped by category. Mirrors the dev-persona backend board, but
  // light. Cards are logo + name + description only -- no CTA buttons.
  function appBoard() {
    var groups = [
      { label: 'Coding agents', kind: 'apps-coding' },
      { label: 'Personal agents', kind: 'apps-personal' },
      { label: 'Productivity', kind: 'apps-productivity' }
    ];
    // One continuous --card index across every element -- group labels included --
    // so labels and their cards fade in together in a smooth top-to-bottom stagger.
    var idx = 0;
    var html = groups.map(function(g) {
      var labelIdx = idx++;
      var cards = (appCategories[g.kind] || []).map(function(app) {
        var card = '<div class="hp-appboard-card" style="--card:' + idx + '">' +
            '<div class="hp-appboard-head">' +
              '<img class="hp-appboard-logo" src="' + APP_LOGO + escapeText(app.id) + '/logo.png" alt="" loading="lazy" />' +
              '<span class="hp-appboard-name">' + escapeText(app.name) + '</span>' +
            '</div>' +
            '<span class="hp-appboard-desc">' + escapeText(app.desc) + '</span>' +
          '</div>';
        idx += 1;
        return card;
      }).join('');
      return '<div class="hp-appboard-grouplabel" style="--card:' + labelIdx + '">' + escapeText(g.label) + '</div>' +
        '<div class="hp-appboard-grid">' + cards + '</div>';
    }).join('');
    return '<div class="hp-appboard">' + html + '</div>';
  }

  // "Add as an MCP server": a generic client's MCP-server settings panel showing the
  // lemonade entry being added to mcp.json (highlighted), a connected status, and the
  // tools it exposes. Mirrors the real /mcp gateway (Streamable HTTP) -- see
  // docs/api/mcp.md: five tools, JSON-RPC over a single POST /mcp endpoint.
  function mcpDemo() {
    var tools = [
      { icon: 'forum', name: 'lemonade_chat', tag: 'chat completion' },
      { icon: 'image', name: 'lemonade_generate_image', tag: 'image generation' },
      { icon: 'graphic_eq', name: 'lemonade_transcribe_audio', tag: 'transcription' },
      { icon: 'auto_awesome', name: 'lemonade_omni', tag: 'multimodal' },
      { icon: 'inventory_2', name: 'lemonade_list_models', tag: 'model discovery' }
    ];
    var toolRows = tools.map(function(t, i) {
      return '<div class="hp-mcp-tool" style="--row:' + i + '">' +
          '<span class="hp-mcp-tool-icon"><span class="material-symbols-outlined">' + t.icon + '</span></span>' +
          '<span class="hp-mcp-tool-text">' +
            '<span class="hp-mcp-tool-name">' + escapeText(t.name) + '</span>' +
            '<span class="hp-mcp-tool-tag">' + escapeText(t.tag) + '</span>' +
          '</span>' +
        '</div>';
    }).join('');
    var fields = [
      { label: 'Name', value: 'lemonade' },
      { label: 'Server URL', value: 'http://localhost:13305/mcp', mono: true },
      { label: 'Transport', value: 'Streamable HTTP', caret: true }
    ];
    var formFields = fields.map(function(f, i) {
      return '<div class="hp-mcp-field-group" style="--row:' + i + '">' +
          '<span class="hp-mcp-label">' + escapeText(f.label) + '</span>' +
          '<div class="hp-mcp-field">' +
            '<span class="hp-mcp-val' + (f.mono ? ' hp-mcp-val-mono' : '') + '">' + escapeText(f.value) + '</span>' +
            (f.caret ? '<span class="material-symbols-outlined hp-mcp-field-caret">expand_more</span>' : '') +
          '</div>' +
        '</div>';
    }).join('');
    var body =
      '<div class="hp-mcp-stage">' +
        '<div class="hp-mcp-scrim"></div>' +
        '<div class="hp-mcp-panel">' +
          '<div class="hp-mcp-col hp-mcp-col-form">' +
            formFields +
            '<button class="hp-mcp-add-btn" type="button"><span class="material-symbols-outlined">add</span>Add server</button>' +
          '</div>' +
          '<div class="hp-mcp-col hp-mcp-col-tools">' +
            '<div class="hp-mcp-status"><span class="hp-mcp-dot"></span>Connected · 5 tools</div>' +
            '<div class="hp-mcp-tools">' + toolRows + '</div>' +
          '</div>' +
        '</div>' +
      '</div>';
    return h.appWindow('MCP Servers', body, 'hp-mcp-window');
  }

  // "Connect any OpenAI-compatible app": a generic client's connection settings.
  // The base URL types itself in, a cursor flies to the Connect button and clicks,
  // then a glowing-green "100+ models found!" status confirms the handshake. The
  // API key field stays empty (a local Lemonade server needs no key by default).
  // Pure-CSS timeline (see .hp-conn-* in persona-demo.css); one-shot on render.
  function connectDemo() {
    var body =
      '<div class="hp-conn-stage">' +
        '<div class="hp-conn-scrim"></div>' +
        '<div class="hp-conn-modal">' +
          '<div class="hp-conn-modal-head">' +
            '<span class="hp-conn-head-title">' +
              '<span class="material-symbols-outlined hp-conn-head-icon">power</span>Add a connection' +
            '</span>' +
            '<span class="material-symbols-outlined hp-conn-modal-close">close</span>' +
          '</div>' +
          '<div class="hp-conn-modal-body">' +
            '<div class="hp-conn-field-group">' +
              '<span class="hp-conn-label">Base URL</span>' +
              '<div class="hp-conn-field">' +
                '<span class="hp-conn-typed">http://localhost:13305</span>' +
                '<span class="hp-conn-caret"></span>' +
              '</div>' +
            '</div>' +
            '<div class="hp-conn-field-group">' +
              '<span class="hp-conn-label">API Key</span>' +
              '<div class="hp-conn-field">' +
                '<span class="hp-conn-placeholder">Optional for local AI</span>' +
              '</div>' +
            '</div>' +
            '<div class="hp-conn-actions">' +
              '<button class="hp-conn-btn" type="button">' +
                '<span class="material-symbols-outlined">link</span>Connect' +
                h.CURSOR_SVG +
              '</button>' +
              '<div class="hp-conn-status">' +
                '<span class="hp-conn-dot"></span>' +
                '<span class="hp-conn-status-text">100+ models found!</span>' +
              '</div>' +
            '</div>' +
          '</div>' +
        '</div>' +
      '</div>';
    return h.appWindow('Any OpenAI Compatible App', body, 'hp-conn-window');
  }

  P.registerDemo('apps-board', function(frame) { frame.innerHTML = appBoard(); });
  P.registerDemo('apps-connect', function(frame) { frame.innerHTML = connectDemo(); });
  P.registerDemo('apps-mcp', function(frame) { frame.innerHTML = mcpDemo(); });
})();
