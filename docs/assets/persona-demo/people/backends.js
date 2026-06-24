// People · section 2 · "Try the backends"
// Slides: install the inference engines, then benchmark them head-to-head.
(function () {
  var P = window.LemonadePersona;
  if (!P) return;
  var h = P.helpers;

  P.registerSection('people', 2, {
    title: 'Try the backends',
    slides: [
      {
        label: 'Install inference engines',
        demo: 'backend-manager',
        caption: 'Download the inference engines you want — FastFlowLM, llama.cpp, Ryzen AI, and vLLM.',
        captionHref: 'https://lemonade-server.ai/docs/embeddable/backends/',
        animationMode: 'once'
      },
      {
        label: 'Benchmark with one command',
        demo: 'terminal-bench',
        caption: 'Compare backends head-to-head on your own hardware with lemonade bench.',
        captionHref: 'https://lemonade-server.ai/docs/guide/cli/',
        animationMode: 'once'
      }
    ]
  });

  // "Try the backends": a backend manager mirroring the model manager -- a "Large
  // Language Models" category, then the four LLM inference engines to download. All
  // four are flagged downloading with staggered --swap-at, so the single cursor
  // (one visible at a time) clicks each download button top-to-bottom in sequence.
  function backendManager() {
    return h.appWindowList({
      title: 'Backend Manager',
      category: 'Large Language Models',
      items: [
        { name: 'FastFlowLM', meta: 'NPU', downloading: true, swapAt: 800 },
        { name: 'llama.cpp', meta: 'GPU · CPU', downloading: true, swapAt: 2200 },
        { name: 'Ryzen AI SW', meta: 'NPU · Hybrid', downloading: true, swapAt: 3600 },
        { name: 'vLLM', meta: 'GPU · ROCm', downloading: true, swapAt: 5000 }
      ]
    });
  }

  var BENCH_LINES = [
    { text: '# Benchmark one model across backends', kind: 'comment', phase: 0, delay: 160 },
    { text: '$ lemonade bench Qwen3.5-4B --backends llamacpp,vllm', kind: 'command', phase: 0, delay: 470 },
    { text: '', delay: 820 },
    { text: '  BACKEND      DEVICE       PROMPT t/s   DECODE t/s', kind: 'output', phase: 1, delay: 1180 },
    { text: '  ───────────────────────────────────────────────', kind: 'output', phase: 1, delay: 1380 },
    { text: '  llama.cpp    Vulkan        ████         ████', kind: 'output', phase: 1, delay: 1700 },
    { text: '  vLLM         ROCm          ████         ████', kind: 'output', phase: 2, delay: 2200 }
  ];

  P.registerDemo('backend-manager', function(frame, o) {
    frame.innerHTML = backendManager();
    if (o.animate) h.playDownloadCursor(frame);
  });
  P.registerDemo('terminal-bench', function(frame) {
    frame.innerHTML = h.renderTerminal('Bash', BENCH_LINES);
  });
})();
