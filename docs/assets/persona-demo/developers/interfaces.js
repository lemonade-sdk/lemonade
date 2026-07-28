// Developers · section 2 · "Standard interfaces"
// Four visual slides about ONE subject: the lemond server and the interfaces it
// exposes. Each renders on a shared dark "server board" (a green ● lemond ·
// :13305 identity bar, no desktop-app chrome) so the metaphor stays consistent —
// these are server interfaces, not apps. Slides 1 & 3 look at the server from the
// inside (its API surface); slides 2 & 4 look at it from the outside (who can call
// it). Cards/pills cascade in with the shared --row stagger.
(function () {
  var P = window.LemonadePersona;
  if (!P) return;
  var h = P.helpers;
  var esc = h.escapeText;

  // Monochrome (warm-cream) brand logos read on the dark board and stay visible
  // regardless of the brand colour — fixes e.g. JavaScript yellow on white.
  function logoUrl(slug) { return 'https://cdn.simpleicons.org/' + esc(slug) + '/f4efdc'; }

  // The shared chrome: a dark board with a "● lemond · http://localhost:13305"
  // identity bar. `tag` is the optional right-aligned label for this facet.
  function board(cls, tag, bodyHtml) {
    return '<div class="hp-iface-board ' + cls + '">' +
        '<div class="hp-iface-server">' +
          '<span class="hp-iface-server-dot"></span>' +
          '<span class="hp-iface-server-name">lemond</span>' +
          '<span class="hp-iface-server-url">http://localhost:13305</span>' +
          (tag ? '<span class="hp-iface-server-tag">' + esc(tag) + '</span>' : '') +
        '</div>' +
        '<div class="hp-iface-body">' + bodyHtml + '</div>' +
      '</div>';
  }

  P.registerSection('developers', 2, {
    title: 'Standard interfaces',
    slides: [
      {
        label: 'OpenAI-compatible',
        demo: 'iface-openai',
        caption: 'Drop-in OpenAI API — point any OpenAI SDK at Lemonade and it just works.',
        captionHref: 'https://lemonade-server.ai/docs/api/openai/',
        animationMode: 'once'
      },
      {
        label: 'Any language',
        demo: 'iface-langs',
        caption: 'It’s just HTTP. Call Lemonade from any language or framework.',
        captionHref: 'https://lemonade-server.ai/docs/api/',
        animationMode: 'repeat'
      },
      {
        label: 'Full server configuration',
        demo: 'iface-control',
        caption: 'Automate the whole server — models, backends, config, lifecycle — over HTTP.',
        captionHref: 'https://lemonade-server.ai/docs/api/lemonade/',
        animationMode: 'once'
      },
      {
        label: 'Ollama & Anthropic',
        demo: 'iface-compat',
        caption: 'Also speaks Ollama and Anthropic, so the apps and SDKs built for them work unchanged.',
        captionHref: 'https://lemonade-server.ai/docs/api/anthropic/',
        animationMode: 'once'
      }
    ]
  });

  // ---- Slide 1: the wall of OpenAI-compatible endpoints --------------------
  var OPENAI_ENDPOINTS = [
    { verb: 'POST', path: '/v1/chat/completions', icon: 'chat' },
    { verb: 'POST', path: '/v1/completions', icon: 'edit_note' },
    { verb: 'POST', path: '/v1/responses', icon: 'forum' },
    { verb: 'POST', path: '/v1/embeddings', icon: 'hub' },
    { verb: 'POST', path: '/v1/reranking', icon: 'sort' },
    { verb: 'POST', path: '/v1/images/generations', icon: 'image' },
    { verb: 'POST', path: '/v1/images/edits', icon: 'auto_fix_high' },
    { verb: 'POST', path: '/v1/images/variations', icon: 'collections' },
    { verb: 'POST', path: '/v1/audio/transcriptions', icon: 'transcribe' },
    { verb: 'POST', path: '/v1/audio/speech', icon: 'record_voice_over' },
    { verb: 'GET', path: '/v1/models', icon: 'list' },
    { verb: 'GET', path: '/v1/models/{id}', icon: 'label' }
  ];

  function openaiWall() {
    var pills = OPENAI_ENDPOINTS.map(function (e, i) {
      return '<span class="hp-iface-pill" style="--row:' + i + '">' +
          '<span class="hp-iface-pill-icon"><span class="material-symbols-outlined">' + esc(e.icon) + '</span></span>' +
          '<span class="hp-iface-verb hp-iface-verb-' + e.verb.toLowerCase() + '">' + esc(e.verb) + '</span>' +
          '<span class="hp-iface-path">' + esc(e.path) + '</span>' +
        '</span>';
    }).join('');
    return board('hp-iface-wall-board', 'OpenAI-compatible',
      '<div class="hp-iface-grid">' + pills + '</div>');
  }

  // ---- Slide 2: language-agnostic HTTP, every client → one server ----------
  // C# / Java logos were pulled from Simple Icons for trademark reasons, so C#
  // rides the .NET (dotnet) mark — the recognizable proxy for the language.
  var LANGS = [
    { slug: 'python', name: 'Python' },
    { slug: 'javascript', name: 'JavaScript' },
    { slug: 'typescript', name: 'TypeScript' },
    { slug: 'dotnet', name: 'C#' },
    { slug: 'go', name: 'Go' },
    { slug: 'rust', name: 'Rust' },
    { slug: 'cplusplus', name: 'C++' },
    { slug: 'swift', name: 'Swift' }
  ];

  function langChip(l, side, idx) {
    var id = '<span class="hp-iface-lang-id">' +
        '<span class="hp-iface-lang-logo"><img src="' + logoUrl(l.slug) + '" alt="' + esc(l.name) + '" loading="lazy" /></span>' +
        '<span class="hp-iface-lang-name">' + esc(l.name) + '</span>' +
      '</span>';
    var wire = '<span class="hp-iface-wire"><span class="hp-iface-pulse" style="--pulse-delay:' + (idx * 0.4) + 's"></span></span>';
    return side === 'left'
      ? '<div class="hp-iface-lang hp-iface-lang-left">' + id + wire + '</div>'
      : '<div class="hp-iface-lang hp-iface-lang-right">' + wire + id + '</div>';
  }

  function langsHub() {
    var left = LANGS.slice(0, 4).map(function (l, i) { return langChip(l, 'left', i); }).join('');
    var right = LANGS.slice(4).map(function (l, i) { return langChip(l, 'right', i); }).join('');
    var hub =
      '<div class="hp-iface-hubserver">' +
        '<span class="hp-iface-hubserver-icon"><span class="material-symbols-outlined">dns</span></span>' +
        '<span class="hp-iface-hubserver-name">lemond</span>' +
        '<span class="hp-iface-hubserver-url">:13305</span>' +
        '<span class="hp-iface-hubserver-tag">HTTP · REST</span>' +
      '</div>';
    var body =
      '<div class="hp-iface-head">Call it from any language</div>' +
      '<div class="hp-iface-langs-stage">' +
        '<div class="hp-iface-langcol">' + left + '</div>' +
        hub +
        '<div class="hp-iface-langcol">' + right + '</div>' +
      '</div>' +
      '<div class="hp-iface-foot"><span class="material-symbols-outlined">bolt</span>If it speaks HTTP, it speaks Lemonade</div>';
    return board('hp-iface-langs-board', 'Any client', body);
  }

  // ---- Slide 3: the server's control plane (admin over HTTP) ---------------
  // Each tile groups real management endpoints as METHOD + path, so it's
  // unmistakable the dev is looking at HTTP endpoints they can call.
  var CONTROL_TILES = [
    { title: 'System', icon: 'monitor_heart', eps: [['GET', '/v1/system-info'], ['GET', '/v1/system-stats']] },
    { title: 'Models', icon: 'inventory_2', eps: [['POST', '/v1/pull'], ['POST', '/v1/load'], ['POST', '/v1/unload'], ['POST', '/v1/delete']] },
    { title: 'Backends', icon: 'memory', eps: [['POST', '/v1/install'], ['POST', '/v1/uninstall']] },
    { title: 'Config & scale', icon: 'tune', eps: [['POST', '/v1/params'], ['POST', '/v1/cloud/auth'], ['POST', '/internal/set']] },
    { title: 'Lifecycle', icon: 'restart_alt', eps: [['GET', '/v1/health'], ['GET', '/v1/logs/stream'], ['POST', '/internal/shutdown']], wide: true }
  ];

  function controlDash() {
    var tiles = CONTROL_TILES.map(function (t, i) {
      var eps = t.eps.map(function (e) {
        return '<span class="hp-iface-ep">' +
            '<span class="hp-iface-ep-verb hp-iface-ep-verb-' + e[0].toLowerCase() + '">' + esc(e[0]) + '</span>' +
            '<span class="hp-iface-ep-path">' + esc(e[1]) + '</span>' +
          '</span>';
      }).join('');
      return '<div class="hp-iface-tile' + (t.wide ? ' hp-iface-tile-wide' : '') + '" style="--row:' + i + '">' +
          '<div class="hp-iface-tile-head">' +
            '<span class="hp-iface-tile-icon"><span class="material-symbols-outlined">' + esc(t.icon) + '</span></span>' +
            '<span class="hp-iface-tile-title">' + esc(t.title) + '</span>' +
          '</div>' +
          '<div class="hp-iface-tile-eps">' + eps + '</div>' +
        '</div>';
    }).join('');
    var body =
      '<div class="hp-iface-head">Manage the whole server with HTTP endpoints</div>' +
      '<div class="hp-iface-dash-grid">' + tiles + '</div>';
    return board('hp-iface-control-board', 'Configuration', body);
  }

  // ---- Slide 4: extra API dialects = more clients work, no adapters --------
  var DIALECTS = [
    {
      slug: 'ollama',
      name: 'Ollama API',
      ep: '/api/chat · /api/generate · /api/tags',
      benefit: 'Every app and tool built for Ollama talks to Lemonade unchanged.'
    },
    {
      slug: 'claude',
      name: 'Anthropic API',
      ep: '/v1/messages · tool use · streaming',
      benefit: 'Anthropic-API clients and SDKs run on local models.'
    }
  ];

  function dialectRow(d, i) {
    return '<div class="hp-iface-dialect" style="--row:' + i + '">' +
        '<span class="hp-iface-dialect-badge"><span class="material-symbols-outlined">check_circle</span>works unchanged</span>' +
        '<span class="hp-iface-dialect-logo"><img src="' + logoUrl(d.slug) + '" alt="' + esc(d.name) + '" loading="lazy" /></span>' +
        '<div class="hp-iface-dialect-text">' +
          '<span class="hp-iface-dialect-name">' + esc(d.name) + '</span>' +
          '<code class="hp-iface-dialect-ep">' + esc(d.ep) + '</code>' +
          '<div class="hp-iface-dialect-benefit">' + esc(d.benefit) + '</div>' +
        '</div>' +
      '</div>';
  }

  function dialectsBoard() {
    var rows = DIALECTS.map(dialectRow).join('');
    var body =
      '<div class="hp-iface-head">Beyond OpenAI — two more drop-in dialects</div>' +
      rows +
      '<div class="hp-iface-foot"><span class="material-symbols-outlined">bolt</span>Reach the whole ecosystem — no adapters to write</div>';
    return board('hp-iface-dialect-board', 'Also speaks', body);
  }

  P.registerDemo('iface-openai', function (frame) { frame.innerHTML = openaiWall(); });
  P.registerDemo('iface-langs', function (frame) { frame.innerHTML = langsHub(); });
  P.registerDemo('iface-control', function (frame) { frame.innerHTML = controlDash(); });
  P.registerDemo('iface-compat', function (frame) { frame.innerHTML = dialectsBoard(); });
})();
