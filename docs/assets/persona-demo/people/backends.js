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
        label: 'Benchmark and compare',
        demo: 'terminal-bench',
        caption: 'Compare backends head-to-head — or sweep real-world scenarios — with lemonade bench.',
        captionHref: 'https://lemonade-server.ai/docs/guide/cli/',
        animationMode: 'once'
      }
    ]
  });

  // "Try the backends": a backend manager mirroring the model manager -- a "Large
  // Language Models" category with the four LLM inference engines, then audio
  // backends grouped by direction (text-to-speech, speech-to-text). Only the four
  // LLM rows are flagged downloading with staggered --swap-at, so the single cursor
  // (one visible at a time) clicks each download button top-to-bottom in sequence;
  // the audio rows sit idle so the animation is unchanged.
  function backendManager() {
    return h.appWindowList({
      title: 'Backend Manager',
      groups: [
        {
          category: 'Large Language Models',
          items: [
            { name: 'FastFlowLM', meta: 'NPU', downloading: true, swapAt: 800 },
            { name: 'llama.cpp', meta: 'GPU · CPU', downloading: true, swapAt: 2200 },
            { name: 'Ryzen AI SW', meta: 'NPU · Hybrid', downloading: true, swapAt: 3600 },
            { name: 'vLLM', meta: 'GPU · ROCm', downloading: true, swapAt: 5000 }
          ]
        },
        {
          category: 'Text to Speech',
          items: [
            { name: 'Kokoro', meta: 'CPU' }
          ]
        },
        {
          category: 'Speech to Text',
          items: [
            { name: 'Moonshine', meta: 'CPU' },
            { name: 'whisper.cpp', meta: 'CPU' }
          ]
        }
      ]
    });
  }

  // Two real `lemonade bench` invocations. The columns mirror the actual results
  // table (src/cpp/cli/bench.cpp: Scenario/TTFT/TPS/VRAM -- there is no "prompt t/s"),
  // collapsed to the three headline means. Bars are deliberately abstract glyphs, not
  // proportional measurements, so the demo claims no specific winner. Block 1 compares
  // engines head-to-head by passing both model variants; block 2 shows the --scenarios
  // feature filtering to the real "coding" category (code-short / code-explain /
  // code-debug from bench_scenarios.json).
  function pad(text, width) {
    text = String(text);
    while (text.length < width) text += ' ';
    return text;
  }
  function benchRow(label, labelW, ttft, tps, vram) {
    return '  ' + pad(label, labelW) + pad(ttft, 9) + pad(tps, 9) + vram;
  }
  var BAR_TTFT = '██', BAR_TPS = '████', BAR_VRAM = '███';

  var BENCH_LINES = [
    // Block 1 -- compare engines head-to-head.
    { text: '# Compare backends head-to-head', kind: 'comment', delay: 160 },
    { text: '$ lemonade bench Qwen3.5-4B-GGUF Qwen3.5-4B-FP16-vLLM', kind: 'command', delay: 470 },
    { text: benchRow('BACKEND', 13, 'TTFT', 'TPS', 'VRAM'), kind: 'output', delay: 980 },
    { text: '  ' + '─'.repeat(35), kind: 'output', delay: 1140 },
    { text: benchRow('llama.cpp', 13, BAR_TTFT, BAR_TPS, BAR_VRAM), kind: 'output', delay: 1340 },
    { text: benchRow('vLLM', 13, BAR_TTFT, BAR_TPS, BAR_VRAM), kind: 'output', delay: 1540 },
    // Block 2 -- focus the run on a scenario category.
    { text: '', delay: 1860 },
    { text: '# Sweep real-world scenarios in one run', kind: 'comment', delay: 2020 },
    { text: '$ lemonade bench Qwen3.5-4B-GGUF --scenarios coding', kind: 'command', delay: 2330 },
    { text: benchRow('SCENARIO', 16, 'TTFT', 'TPS', 'VRAM'), kind: 'output', delay: 2840 },
    { text: '  ' + '─'.repeat(38), kind: 'output', delay: 3000 },
    { text: benchRow('code-short', 16, BAR_TTFT, BAR_TPS, BAR_VRAM), kind: 'output', delay: 3200 },
    { text: benchRow('code-explain', 16, BAR_TTFT, BAR_TPS, BAR_VRAM), kind: 'output', delay: 3400 },
    { text: benchRow('code-debug', 16, BAR_TTFT, BAR_TPS, BAR_VRAM), kind: 'output', delay: 3600 }
  ];

  P.registerDemo('backend-manager', function(frame, o) {
    frame.innerHTML = backendManager();
    if (o.animate) h.playDownloadCursor(frame);
  });
  P.registerDemo('terminal-bench', function(frame) {
    frame.innerHTML = h.renderTerminal('Bash', BENCH_LINES);
  });
})();
