// Developers · section 3 · "Backends and devices"
// Slides: the inference engines board + the devices/acceleration board.
(function () {
  var P = window.LemonadePersona;
  if (!P) return;
  var h = P.helpers;
  var escapeText = h.escapeText;

  P.registerSection('developers', 3, {
    title: 'Backends and devices',
    slides: [
      {
        label: 'Inference engines',
        demo: 'backend-engines',
        caption: 'One API, many engines — chat, image, speech, and more.',
        captionHref: 'https://lemonade-server.ai/docs/embeddable/backends/',
        animationMode: 'once'
      },
      {
        label: 'Devices & acceleration',
        demo: 'backend-devices',
        caption: 'Optimized for every device — GPU, NPU, and CPU.',
        captionHref: 'https://lemonade-server.ai/docs/guide/configuration/llamacpp/',
        animationMode: 'once'
      }
    ]
  });

  // Developer "Backends and devices": grids of glass icon-cards showing the breadth
  // of engines / device acceleration. Dark variant. Material icons (no external logos).
  var ENGINES_BOARD = [
    { label: 'Core', items: [
      { icon: 'forum', name: 'llama.cpp', tag: 'chat · embed · rerank' },
      { icon: 'image', name: 'stable-diffusion.cpp', tag: 'image' },
      { icon: 'graphic_eq', name: 'whisper.cpp', tag: 'transcription' },
      { icon: 'campaign', name: 'Kokoro', tag: 'text-to-speech' },
      { icon: 'mic', name: 'Moonshine', tag: 'streaming ASR' }
    ] },
    { label: 'Specialized', items: [
      { icon: 'bolt', name: 'vLLM', tag: 'LLM · ROCm' },
      { icon: 'memory', name: 'RyzenAI', tag: 'NPU' },
      { icon: 'developer_board', name: 'FastFlowLM', tag: 'NPU · multimodal' }
    ] }
  ];
  var DEVICES_BOARD = [
    { items: [
      { icon: 'view_in_ar', name: 'Vulkan', tag: 'any GPU' },
      { icon: 'memory', name: 'ROCm', tag: 'AMD' },
      { icon: 'memory', name: 'CUDA', tag: 'NVIDIA' },
      { icon: 'laptop_mac', name: 'Metal', tag: 'Apple' },
      { icon: 'developer_board', name: 'CPU', tag: 'x86 · ARM' },
      { icon: 'bolt', name: 'NPU', tag: 'Ryzen AI' }
    ] }
  ];

  // Render a board's groups (no window chrome -- the board itself is the dark
  // surface): an optional group label + a grid of cards, --card running across groups.
  function renderBoard(groups) {
    var idx = 0;
    var html = groups.map(function(g) {
      var cards = g.items.map(function(it) {
        var card = '<div class="hp-backend-card" style="--card:' + idx + '">' +
            '<span class="hp-backend-icon"><span class="material-symbols-outlined">' + it.icon + '</span></span>' +
            '<span class="hp-backend-name">' + escapeText(it.name) + '</span>' +
            '<span class="hp-backend-tag">' + escapeText(it.tag) + '</span>' +
          '</div>';
        idx += 1;
        return card;
      }).join('');
      return (g.label ? '<div class="hp-backend-grouplabel">' + escapeText(g.label) + '</div>' : '') +
        '<div class="hp-backend-grid">' + cards + '</div>';
    }).join('');
    return '<div class="hp-backend-board">' + html + '</div>';
  }

  P.registerDemo('backend-engines', function(frame) { frame.innerHTML = renderBoard(ENGINES_BOARD); });
  P.registerDemo('backend-devices', function(frame) { frame.innerHTML = renderBoard(DEVICES_BOARD); });
})();
