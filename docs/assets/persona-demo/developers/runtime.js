// Developers · section 0 · "Embedded SDK"
// Slides: spawn lemond as a subprocess + deploy everywhere (both flowcharts), then
// "own the whole stack" — a private, branded app window.
(function () {
  var P = window.LemonadePersona;
  if (!P) return;
  var h = P.helpers;
  var escapeText = h.escapeText;

  P.registerSection('developers', 0, {
    title: 'Embedded SDK',
    slides: [
      {
        label: 'Start lemond subprocess',
        demo: 'spawn-app',
        caption: 'Your app starts lemond as a subprocess to access the inference stack.',
        captionHref: 'https://lemonade-server.ai/docs/embeddable/',
        animationMode: 'none'
      },
      {
        label: 'Deploy everywhere',
        demo: 'deploy-everywhere',
        caption: 'Any OS, any vendor — lemonade runs everywhere.',
        captionHref: 'https://lemonade-server.ai/docs/embeddable/',
        animationMode: 'repeat'
      },
      {
        label: 'Own the whole stack',
        demo: 'private-app',
        caption: 'Bundle private models and backends, lock them to your API key, and keep every spotlight on your app.',
        captionHref: 'https://lemonade-server.ai/docs/embeddable/',
        animationMode: 'once'
      }
    ]
  });

  // Developer "Own the whole stack": an app window branded as the APP (Your App)
  // commanding its own private lemond — a control chip showing the stack is gated by
  // the dev's API key, a private library of bundled models AND backends (each locked),
  // and a footer keeping the spotlight on the dev's brand. Dark variant to match the
  // developer demos. (Real basis: models_dir, LEMONADE_API_KEY, custom backend_versions.)
  function privateApp() {
    // Two columns of cards — models (by modality) + backends (by device). No per-row
    // locks or stretched tags: the key chip and "Private" headers carry the locked-down
    // meaning. Each card is an icon tile + name + sub-label; the grid and card stacks
    // flex to fill the fixed-height window so there's no dead space.
    var models = [
      { name: 'assistant-7b', sub: 'chat', icon: 'chat' },
      { name: 'vision-4b', sub: 'vision', icon: 'visibility' },
      { name: 'voice-tiny', sub: 'speech', icon: 'mic' }
    ];
    var backends = [
      { name: 'llama.cpp', sub: 'GPU', icon: 'forum' },
      { name: 'FastFlowLM', sub: 'NPU', icon: 'developer_board' },
      { name: 'whisper.cpp', sub: 'CPU', icon: 'graphic_eq' }
    ];
    function cards(list, offset) {
      return list.map(function(it, i) {
        return '<div class="hp-private-card" style="--row:' + (offset + i) + '">' +
            '<span class="hp-private-card-icon"><span class="material-symbols-outlined">' + escapeText(it.icon) + '</span></span>' +
            '<span class="hp-private-card-text">' +
              '<span class="hp-private-name">' + escapeText(it.name) + '</span>' +
              '<span class="hp-private-sub">' + escapeText(it.sub) + '</span>' +
            '</span>' +
          '</div>';
      }).join('');
    }
    // One cohesive group centered in the window's vertical middle: a key-chip header,
    // the two private-stack columns, and a brand line — bracketed top and bottom so the
    // composition reads as a single balanced block, not content scattered to the edges.
    var body =
      '<div class="hp-private">' +
        '<div class="hp-private-keychip"><span class="material-symbols-outlined">key</span>Secured with your API key</div>' +
        '<div class="hp-private-cols">' +
          '<div class="hp-private-col">' +
            '<div class="hp-private-libhead">Private models</div>' +
            '<div class="hp-private-items">' + cards(models, 0) + '</div>' +
          '</div>' +
          '<div class="hp-private-col">' +
            '<div class="hp-private-libhead">Private backends</div>' +
            '<div class="hp-private-items">' + cards(backends, 3) + '</div>' +
          '</div>' +
        '</div>' +
        '<div class="hp-private-foot"><span class="material-symbols-outlined">auto_awesome</span>The spotlight stays on your app</div>' +
      '</div>';
    return h.appWindow('Your App', body, 'hp-private-window is-dark');
  }

  P.registerDemo('spawn-app', function(frame) { h.renderFlowchart(frame, 'spawn-app'); });
  P.registerDemo('deploy-everywhere', function(frame) { h.renderFlowchart(frame, 'deploy-everywhere'); });
  P.registerDemo('private-app', function(frame) { frame.innerHTML = privateApp(); });
})();
