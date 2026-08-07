// People · section 0 · "Explore AI models"
// Slides: the converged omni chatbot, the Lemonade model registry, Hugging Face
// search, and importing local GGUF models.
(function () {
  var P = window.LemonadePersona;
  if (!P) return;
  var h = P.helpers;
  var escapeText = h.escapeText;

  P.registerSection('people', 0, {
    title: 'Explore AI models',
    slides: [
      {
        label: 'Chat, image, code & speech',
        demo: 'explore-omni',
        caption: 'One private, local conversation — chat, image generation, coding, and speech, all in the same app.'
      },
      {
        label: 'Pull from the Lemonade registry',
        demo: 'models-registry',
        caption: 'Lemonade recommends the best new models as they release.',
        captionHref: 'https://lemonade-server.ai/models.html',
        animationMode: 'once'
      },
      {
        label: 'Search on Hugging Face',
        demo: 'models-hf-search',
        caption: 'Most models on huggingface.co can be imported to Lemonade.',
        captionHref: 'https://lemonade-server.ai/docs/guide/configuration/custom-models/',
        animationMode: 'once'
      },
      {
        label: 'Import your models',
        demo: 'terminal-models-import',
        caption: 'Lemonade can import GGUF models already on your PC.',
        captionHref: 'https://lemonade-server.ai/docs/embeddable/models/',
        animationMode: 'once'
      }
    ]
  });

  // Converged "Explore" demo: ONE chatbot window whose transcript plays out every
  // modality in a single conversation -- chat, then image gen, then coding, then
  // speech. playOmni() reveals each turn in sequence, scrolling the feed up so the
  // newest turn stays in view (a real growing-chat feel). One-shot on render.
  function omniDemo() {
    var waveBars = new Array(26).join('<span></span>'); // 25 bars, matching the hero waveform
    var turns = [
      '<div class="hp-chat-user">What can I do with 128 GB of unified RAM?</div>',
      '<div class="hp-chat-ai">Load up models like Qwen-Coder-Next for advanced tool use.</div>',
      '<div class="hp-chat-user">Now paint a pitcher of lemonade</div>',
      '<div class="hp-demo-image-placeholder"></div>',
      '<div class="hp-chat-user">Build a real-time dashboard that streams GPU metrics</div>',
      '<pre class="hp-code-block"><code>' +
        '<span class="hp-code-kw">async def</span> <span class="hp-code-fn">stream_gpu_metrics</span>(ws):\n' +
        '    <span class="hp-code-kw">while</span> <span class="hp-code-lit">True</span>:\n' +
        '        stats = <span class="hp-code-kw">await</span> gpu.poll()\n' +
        '        <span class="hp-code-kw">await</span> ws.send_json(stats)\n' +
        '        <span class="hp-code-kw">await</span> asyncio.sleep(<span class="hp-code-lit">0.5</span>)\n' +
        '<span class="hp-code-ellipsis">...</span>' +
      '</code></pre>',
      '<div class="hp-chat-user">Now voice a welcome message for the dashboard</div>',
      '<div class="hp-waveform">' + waveBars + '</div>'
    ];
    var feed = '<div class="hp-omni-feed">' + turns.map(function(t) {
      return '<div class="hp-omni-turn">' + t + '</div>';
    }).join('') + '</div>';
    return h.appWindow('Lemonade',
      '<div class="hp-chatbot-body">' + feed + '</div>' +
      '<div class="hp-chatbot-input">' +
        '<div class="hp-chatbot-field"><span class="hp-omni-placeholder">Ask anything — text, images, code, or speech…</span></div>' +
        '<span class="hp-chatbot-send"><span class="material-symbols-outlined">arrow_upward</span></span>' +
      '</div>',
      'hp-chatbot hp-omni');
  }

  // Drive the omni transcript: reveal each turn on a timer, then translate the feed
  // up just enough to keep the freshly revealed turn in view. Measures real layout
  // (offsetTop/offsetHeight) so it stays correct regardless of bubble wrapping.
  // Runs once per render. Deliberately ignores prefers-reduced-motion -- these
  // journey demos are core showcase content and must play on every machine (see
  // the policy note in persona-demo.css).
  function playOmni(frameEl) {
    var body = frameEl.querySelector('.hp-chatbot-body');
    var feed = frameEl.querySelector('.hp-omni-feed');
    if (!body || !feed) return;
    var turns = feed.querySelectorAll('.hp-omni-turn');
    if (!turns.length) return;
    // ms to dwell before the NEXT turn arrives (longer after image/code responses).
    var dwell = [700, 1150, 1300, 1550, 1300, 1650, 1300, 1500];
    var t = 450;
    for (var i = 0; i < turns.length; i++) {
      (function(turn) {
        window.setTimeout(function() {
          turn.classList.add('is-in');
          // Rest gap below the freshly revealed turn -- larger than the bubble's own
          // margin so each new turn settles a little higher than the viewport's
          // bottom edge, giving it room to breathe instead of hugging the floor.
          var restGap = 48;
          var overflow = turn.offsetTop + turn.offsetHeight - body.clientHeight + restGap;
          feed.style.transform = 'translateY(' + (overflow > 0 ? -overflow : 0) + 'px)';
        }, t);
      })(turns[i]);
      t += dwell[i] || 1200;
    }
  }

  // models-registry: curated snapshot of suggested/"hot" models from
  // src/cpp/resources/server_models.json, ordered newest-released first.
  function modelsRegistry() {
    return h.appWindowList({
      title: 'Model Manager',
      items: [
        { name: 'Qwen3.6-35B-A3B', meta: '23.3 GB · vision' },
        { name: 'Gemma-4-31B-it', meta: '19.5 GB · vision', downloading: true, swapAt: 1200 },
        { name: 'GLM-4.7-Flash', meta: '17.5 GB · tools' },
        { name: 'Qwen3.5-4B', meta: '3.58 GB · vision', downloading: true, swapAt: 2700 },
        { name: 'gpt-oss-20b', meta: '12.1 GB · reasoning' }
      ]
    });
  }

  // Hugging Face search: the user types a generic model name; real GGUF results
  // appear (live data from huggingface.co, hardcoded here). The cursor clicks the
  // quantization dropdown of the top result (it opens with real quant variants),
  // then clicks download and the progress bar fills. CSS sequences the timing.
  function modelSearch() {
    var query = 'qwen3 coder';
    var quants = ['Q3_K_M', 'Q4_K_M', 'Q5_K_M', 'Q6_K', 'Q8_0'];
    var selected = 'Q4_K_M';
    // Most-downloaded GGUF repos matching "qwen3 coder" on Hugging Face.
    var results = [
      { repo: 'unsloth/Qwen3-Coder-Next-GGUF', dls: '301K', selected: true },
      { repo: 'unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF', dls: '238K' },
      { repo: 'Qwen/Qwen3-Coder-Next-GGUF', dls: '28K' }
    ];
    var menu = quants.map(function(q) {
      return '<span class="hp-quant-opt' + (q === selected ? ' is-active' : '') + '">' + escapeText(q) + '</span>';
    }).join('');
    var rows = results.map(function(r, i) {
      var repo = '<span class="hp-modelsearch-repo">' +
          '<span class="hp-modelsearch-name">' + escapeText(r.repo) + '</span>' +
          '<span class="hp-modelsearch-dls"><span class="material-symbols-outlined">download</span>' + escapeText(r.dls) + '</span>' +
        '</span>';
      if (r.selected) {
        return '<div class="hp-modelsearch-result is-selected" style="--res:' + i + '">' +
            repo +
            '<span class="hp-modelsearch-controls">' +
              '<span class="hp-modelsearch-quant">' +
                '<span class="hp-modelsearch-quant-value">' + escapeText(selected) + '<span class="material-symbols-outlined">expand_more</span></span>' +
                '<span class="hp-modelsearch-quant-menu">' + menu + '</span>' +
              '</span>' +
              '<span class="hp-applist-dl hp-modelsearch-dl"><span class="material-symbols-outlined">download</span></span>' +
            '</span>' +
            '<span class="hp-modelsearch-progress" style="--p:100%"><i></i></span>' +
            h.CURSOR_SVG +
          '</div>';
      }
      return '<div class="hp-modelsearch-result" style="--res:' + i + '">' +
          repo +
          '<span class="hp-applist-dl"><span class="material-symbols-outlined">download</span></span>' +
        '</div>';
    }).join('');
    var body =
      '<div class="hp-modelsearch">' +
        '<div class="hp-modelsearch-bar">' +
          '<span class="material-symbols-outlined hp-modelsearch-icon">search</span>' +
          '<span class="hp-modelsearch-typed">' + escapeText(query) + '</span>' +
          '<span class="hp-modelsearch-caret"></span>' +
        '</div>' +
        '<div class="hp-modelsearch-results">' + rows + '</div>' +
      '</div>';
    return h.appWindow('Model Manager', body, 'hp-modelsearch-window');
  }

  var IMPORT_LINES = [
    { text: '# Point Lemonade at GGUF models already on your PC', kind: 'comment', phase: 0, delay: 160 },
    { text: '$ lemonade config set extra_models_dir="/path/to/models"', kind: 'command', phase: 0, delay: 470 },
    { text: '✓ configuration updated', kind: 'output', phase: 0, delay: 820 },
    { text: '', delay: 1120 },
    { text: '$ lemonade list | grep custom', kind: 'command', phase: 1, delay: 1420 },
    { text: '✓ imported Qwen3.6-35B-A3B-GGUF', kind: 'output', phase: 1, delay: 1820 },
    { text: '✓ imported gpt-oss-120b-GGUF', kind: 'output', phase: 1, delay: 2080 },
    { text: '✓ imported GLM-4.7-Flash-GGUF', kind: 'output', phase: 1, delay: 2340 }
  ];

  P.registerDemo('explore-omni', function(frame, o) {
    frame.innerHTML = omniDemo();
    if (o.animate) playOmni(frame);
  });
  P.registerDemo('models-registry', function(frame, o) {
    frame.innerHTML = modelsRegistry();
    if (o.animate) h.playDownloadCursor(frame);
  });
  P.registerDemo('models-hf-search', function(frame, o) {
    frame.innerHTML = modelSearch();
    if (o.animate) h.playDownloadCursor(frame);
  });
  P.registerDemo('terminal-models-import', function(frame) {
    frame.innerHTML = h.renderTerminal('Bash', IMPORT_LINES);
  });
})();
