// Developers · section 2 · "Standard interfaces"
// Slides: the OpenAI API, the Lemonade API, and Ollama/Anthropic compatibility.
(function () {
  var P = window.LemonadePersona;
  if (!P) return;
  var h = P.helpers;

  P.registerSection('developers', 2, {
    title: 'Standard interfaces',
    slides: [
      {
        label: 'OpenAI API',
        demo: 'terminal-api-openai',
        caption: 'Chat, image, and speech with industry-standard APIs.',
        captionHref: 'https://lemonade-server.ai/docs/api/openai/',
        animationMode: 'once'
      },
      {
        label: 'Lemonade API',
        demo: 'terminal-api-lemonade',
        caption: 'Manage models, backends, configuration, and lemond lifecycle.',
        captionHref: 'https://lemonade-server.ai/docs/api/lemonade/',
        animationMode: 'once'
      },
      {
        label: 'Ollama & Anthropic API',
        demo: 'terminal-api-compat',
        caption: 'Bring any Ollama client — or Anthropic-API apps like Claude Code — to local models.',
        captionHref: 'https://lemonade-server.ai/docs/api/anthropic/',
        animationMode: 'once'
      }
    ]
  });

  var OPENAI_LINES = [
    { text: '# Chat · image · speech over the OpenAI API', kind: 'comment', phase: 0, delay: 160 },
    { text: '$ curl :13305/v1/chat/completions -d \'{"messages":[{"role":"user","content":"Population of Paris?"}]}\'', kind: 'command', phase: 0, delay: 480 },
    { text: '{ "content": "Paris has about 2.2 million residents." }', kind: 'output', phase: 0, delay: 820 },
    { text: '', delay: 1120 },
    { text: '$ curl :13305/v1/images/generations -d \'{"model":"SD-Turbo","prompt":"a lemon grove"}\'', kind: 'command', phase: 1, delay: 1420 },
    { text: '✓ generated 512×512 PNG', kind: 'output', phase: 1, delay: 1740 },
    { text: '', delay: 2000 },
    { text: '$ curl :13305/v1/audio/speech -d \'{"model":"kokoro-v1","input":"Lemonade can speak!"}\'', kind: 'command', phase: 2, delay: 2300 },
    { text: '✓ speech.mp3 (1.2s)', kind: 'output', phase: 2, delay: 2620 }
  ];

  var LEMONADE_LINES = [
    { text: '# Manage models, backends, and configuration', kind: 'comment', phase: 0, delay: 160 },
    { text: '$ curl :13305/api/v1/pull -d \'{"model_name":"Qwen3-4B-GGUF"}\'', kind: 'command', phase: 0, delay: 480 },
    { text: '✓ installed Qwen3-4B-GGUF', kind: 'output', phase: 0, delay: 820 },
    { text: '', delay: 1100 },
    { text: '$ curl :13305/api/v1/install -d \'{"recipe":"llamacpp","backend":"vulkan"}\'', kind: 'command', phase: 1, delay: 1400 },
    { text: '✓ backend ready: llamacpp:vulkan', kind: 'output', phase: 1, delay: 1740 },
    { text: '', delay: 2000 },
    { text: '$ curl :13305/internal/set -d \'{"max_loaded_models":3}\'', kind: 'command', phase: 2, delay: 2300 },
    { text: '✓ max_loaded_models = 3', kind: 'output', phase: 2, delay: 2620 }
  ];

  var COMPAT_LINES = [
    { text: '# Your Ollama tools, pointed at Lemonade — unchanged', kind: 'comment', phase: 0, delay: 160 },
    { text: '$ OLLAMA_HOST=localhost:13305 ollama run qwen3', kind: 'command', phase: 0, delay: 520 },
    { text: '>>> Hello! Running 100% local on your GPU.', kind: 'output', phase: 0, delay: 900 },
    { text: '', delay: 1200 },
    { text: '# Anthropic API — point Claude Code at local models', kind: 'comment', phase: 1, delay: 1600 },
    { text: '$ ANTHROPIC_BASE_URL=http://localhost:13305 claude', kind: 'command', phase: 1, delay: 2000 },
    { text: '● Qwen3-Coder · 100% local · $0 / token', kind: 'output', phase: 1, delay: 2380 },
    { text: '● edited app.py · ran tests ✓ 8 passed', kind: 'output', phase: 2, delay: 2820 }
  ];

  P.registerDemo('terminal-api-openai', function(frame) { frame.innerHTML = h.renderTerminal('Bash', OPENAI_LINES); });
  P.registerDemo('terminal-api-lemonade', function(frame) { frame.innerHTML = h.renderTerminal('Bash', LEMONADE_LINES); });
  P.registerDemo('terminal-api-compat', function(frame) { frame.innerHTML = h.renderTerminal('Bash', COMPAT_LINES); });
})();
