// ============================================================================
// Persona-demo hero
// ----------------------------------------------------------------------------
// Drives the homepage persona content: an overview MAP (people = journey path,
// developers = stack diagram) followed by the steps UNROLLED into one scroll
// section each. Each section's demo loops while it is in view (an Intersection
// Observer starts/stops a per-section timer that re-renders the current slide)
// and freezes off-screen. The map collapses into a sticky progress bar as you
// scroll. This module OWNS the persona state (the data-persona attribute, the dev
// dark scheme, persistence, and the toggle-only lemonadePersonaChange signal).
//
// The flowchart diagrams live in flowchart.js (window.LemonadeFlowchart); this
// module hands that renderer the loop cadence. Requires flowchart.js first.
// ============================================================================
(function () {
  // The hero + promise are mission-level and static (authored in index.html); they
  // stay identical across personas, so this module never touches them. Only the
  // zone heading and the journey below it are persona-aware.
  var journeyEl = document.getElementById('personaJourney');
  var zoneEl = document.getElementById('personaZoneHeading');
  var zoneSubtitleEl = document.getElementById('personaZoneSubtitle');
  if (!journeyEl) return;

  // Flowchart animation cadence (passed through to flowchart.js render()).
  var defaultAutoplayDelay = 5200;       // min cycle length
  var animationSubsectionDelay = 2450;   // per-subsection duration
  var animationSubsectionGap = 350;
  var STEP_ICONS = ['explore', 'apps', 'terminal', 'dns'];  // people steps
  var personaSteps = {
    people: {
      title: 'Run AI on your personal hardware.',
      subtitle: 'Lemonade is a refreshingly simple, free and open-source way to run AI locally. It optimizes for your device, stays private, and works from desktop, server, or mobile.',
      zone: 'Get to know Lemonade',
      zoneSubtitle: 'From your first chat to your own self-hosted server — see everything you can do with local AI on your hardware.',
      label: 'User journey',
      steps: [
        {
          eyebrow: 'Explore',
          title: 'Explore AI models',
          copy: 'Run chat, image, coding, and speech models locally — and manage your whole model library from one app.',
          demo: 'explore-omni',
          slides: [
            {
              label: 'Chat, image, code & speech',
              demo: 'explore-omni',
              caption: 'One private, local conversation — chat, image generation, coding, and speech, all in the same app.',
              duration: 9000
            },
            {
              label: 'Pull from the Lemonade registry',
              demo: 'models-registry',
              caption: 'Lemonade recommends the best new models as they release.',
              captionHref: 'https://lemonade-server.ai/models.html',
              animationMode: 'once',
              duration: 5600
            },
            {
              label: 'Search on Hugging Face',
              demo: 'models-hf-search',
              caption: 'Most models on huggingface.co can be imported to Lemonade.',
              captionHref: 'https://lemonade-server.ai/docs/guide/configuration/custom-models/',
              animationMode: 'once',
              duration: 5800
            },
            {
              label: 'Import your models',
              demo: 'terminal-models-import',
              caption: 'Lemonade can import GGUF models already on your PC.',
              captionHref: 'https://lemonade-server.ai/docs/embeddable/models/',
              animationMode: 'once',
              duration: 4000
            }
          ]
        },
        {
          eyebrow: 'Apps',
          title: 'Connect to apps',
          copy: 'Hundreds of great AI apps connect to Lemonade.',
          demo: 'apps-board',
          slides: [
            {
              label: 'Featured apps',
              demo: 'apps-board',
              caption: 'From coding agents to productivity tools — connect the apps you already love, with no API costs.',
              captionHref: 'https://lemonade-server.ai/marketplace.html',
              animationMode: 'once',
              duration: 4200
            },
            {
              label: 'Add as an MCP server',
              demo: 'apps-mcp',
              caption: 'Expose your local models as MCP tools any compatible client can call.',
              captionHref: 'https://lemonade-server.ai/docs/api/mcp/',
              animationMode: 'once',
              duration: 4200
            }
          ]
        },
        {
          eyebrow: 'CLI',
          title: 'Learn the CLI',
          copy: 'Everything Lemonade does is one command away.',
          demo: 'terminal-cli-chat',
          slides: [
            {
              label: 'Chat REPL',
              demo: 'terminal-cli-chat',
              caption: 'Chat with any model right in your terminal.',
              captionHref: 'https://lemonade-server.ai/docs/guide/cli-chat/',
              animationMode: 'once',
              duration: 3800
            },
            {
              label: 'List backends',
              demo: 'terminal-cli-backends',
              caption: 'See every inference backend available on your machine.',
              captionHref: 'https://lemonade-server.ai/docs/guide/cli/',
              animationMode: 'once',
              duration: 3400
            },
            {
              label: 'Set configuration',
              demo: 'terminal-cli-config',
              caption: 'Tune the server straight from the command line.',
              captionHref: 'https://lemonade-server.ai/docs/guide/cli/',
              animationMode: 'once',
              duration: 3000
            }
          ]
        },
        {
          eyebrow: 'Self-host',
          title: 'Self-hosted AI',
          copy: 'Serve free, private AI to your whole household and beyond.',
          demo: 'terminal-selfhost',
          slides: [
            {
              label: 'Secure it and go LAN-wide',
              demo: 'terminal-selfhost',
              caption: 'Set an API key, then bind to 0.0.0.0 to serve every device on your network.',
              captionHref: 'https://lemonade-server.ai/docs/guide/configuration/',
              animationMode: 'once',
              duration: 4200
            },
            {
              label: 'Serve the whole household',
              demo: 'apps-selfhost',
              caption: 'Give everyone polished, private AI with Open WebUI and Dream Server.',
              captionHref: 'https://lemonade-server.ai/docs/integrations/open-webui/',
              animationMode: 'once',
              duration: 3600
            }
          ]
        }
      ]
    },
    developers: {
      title: 'One router. Every backend. Any app.',
      subtitle: 'Embed lemond behind one clean OpenAI-compatible API and ship local AI across CPU, GPU, NPU, RAM, and cloud.',
      zone: 'Dive into the software stack',
      zoneSubtitle: 'From the embedded runtime up through the router, APIs, and backends that power local AI in your app.',
      label: 'Software stack',
      steps: [
        {
          eyebrow: 'Runtime',
          title: 'Embedded SDK',
          copy: 'A complete inference stack for your app in a tiny 3 MB portable binary.',
          demo: 'spawn-app',
          slides: [
            {
              label: 'Start lemond subprocess',
              demo: 'spawn-app',
              caption: 'Your app starts lemond as a subprocess to access the inference stack.',
              captionHref: 'https://lemonade-server.ai/docs/embeddable/',
              animationMode: 'none',
              duration: 3600
            },
            {
              label: 'Deploy everywhere',
              demo: 'deploy-everywhere',
              caption: 'Compatible with every mainstream PC.',
              captionHref: 'https://lemonade-server.ai/docs/embeddable/',
              animationMode: 'repeat',
              duration: 5200
            },
            {
              label: 'Private & white-labeled',
              demo: 'private-app',
              caption: 'Ship your own inference stack — your models, your key, no Lemonade branding.',
              captionHref: 'https://lemonade-server.ai/docs/embeddable/',
              animationMode: 'once',
              duration: 3400
            }
          ]
        },
        {
          eyebrow: 'Routing',
          title: 'Smart Router',
          copy: 'lemond intelligently selects between chat, image, speech, local, and cloud.',
          demo: 'router-omni',
          slides: [
            {
              label: 'Omni models',
              demo: 'router-omni',
              caption: 'Send and receive multimedia with virtual omni models.',
              captionHref: 'https://lemonade-server.ai/docs/dev/omni-router/',
              animationMode: 'repeat',
              animationSections: ['text to image', 'speech transcription']
            },
            {
              label: 'Cloud/local hybrid',
              demo: 'router-hybrid',
              caption: 'Cloud models when needed, local by default.',
              captionHref: 'https://lemonade-server.ai/docs/guide/configuration/cloud/',
              animationMode: 'repeat',
              animationSections: ['small local route', 'cloud route']
            }
          ]
        },
        {
          eyebrow: 'Interfaces',
          title: 'Standard interfaces',
          copy: 'All capabilities available over language-agnostic HTTP endpoints.',
          demo: 'terminal-api-openai',
          slides: [
            {
              label: 'OpenAI API',
              demo: 'terminal-api-openai',
              caption: 'Chat, image, and speech with industry-standard APIs.',
              captionHref: 'https://lemonade-server.ai/docs/api/openai/',
              animationMode: 'once',
              duration: 4200
            },
            {
              label: 'Lemonade API',
              demo: 'terminal-api-lemonade',
              caption: 'Manage models, backends, configuration, and lemond lifecycle.',
              captionHref: 'https://lemonade-server.ai/docs/api/lemonade/',
              animationMode: 'once',
              duration: 4000
            },
            {
              label: 'Ollama API',
              demo: 'terminal-api-ollama',
              caption: 'Point any Ollama client at Lemonade.',
              captionHref: 'https://lemonade-server.ai/docs/api/ollama/',
              animationMode: 'once',
              duration: 3400
            },
            {
              label: 'Anthropic API',
              demo: 'terminal-api-anthropic',
              caption: 'The Anthropic Messages API, with tool use.',
              captionHref: 'https://lemonade-server.ai/docs/api/anthropic/',
              animationMode: 'once',
              duration: 3000
            }
          ]
        },
        {
          eyebrow: 'Backends',
          title: 'Backends and devices',
          copy: 'Provides optimized inference across modalities and platforms.',
          demo: 'backend-engines',
          slides: [
            {
              label: 'Inference engines',
              demo: 'backend-engines',
              caption: 'One API, many engines — chat, image, speech, and more.',
              captionHref: 'https://lemonade-server.ai/docs/embeddable/backends/',
              animationMode: 'once',
              duration: 3600
            },
            {
              label: 'Devices & acceleration',
              demo: 'backend-devices',
              caption: 'Optimized for every device — GPU, NPU, and CPU.',
              captionHref: 'https://lemonade-server.ai/docs/guide/configuration/llamacpp/',
              animationMode: 'once',
              duration: 3200
            }
          ]
        },
        {
          eyebrow: 'Customize',
          title: 'Customize completely',
          copy: 'Configure every aspect of lemond for your app.',
          demo: 'terminal-customize-runtime',
          slides: [
            {
              label: 'Tune at runtime',
              demo: 'terminal-customize-runtime',
              caption: 'Set models, context, and backends from the command line or live API.',
              captionHref: 'https://lemonade-server.ai/docs/guide/configuration/',
              animationMode: 'once',
              duration: 4000
            },
            {
              label: 'Bundle for deployment',
              demo: 'terminal-customize-bundle',
              caption: 'Bundle private models and backends, then launch hidden and locked to your app.',
              captionHref: 'https://lemonade-server.ai/docs/embeddable/',
              animationMode: 'once',
              duration: 3600
            }
          ]
        }
      ]
    }
  };

  function escapeText(text) {
    return String(text).replace(/[&<>"']/g, function(ch) {
      return ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[ch];
    });
  }

  function currentPersona() {
    return document.documentElement.getAttribute('data-persona') || 'people';
  }

  function readStoredPersona() {
    try { return localStorage.getItem('lemonade-persona') || 'people'; }
    catch (e) { return 'people'; }
  }

  // Apply persona to <html> (drives all [data-persona] CSS + the dev dark scheme).
  // Pure state: it does NOT rebuild or signal — callers decide. Used silently on
  // load so no reveal replay fires for the initial persona.
  function applyPersona(persona, persist) {
    var next = persona === 'developers' ? 'developers' : 'people';
    document.documentElement.setAttribute('data-persona', next);
    if (next === 'developers') {
      document.documentElement.setAttribute('data-md-color-scheme', 'zest-dark');
    } else {
      document.documentElement.removeAttribute('data-md-color-scheme');
    }
    if (persist) {
      try { localStorage.setItem('lemonade-persona', next); } catch (e) {}
    }
    return next;
  }

  // The single entry point for a USER-driven switch (hero toggle): apply + persist,
  // rebuild the journey/zone, then emit the toggle-only lemonadePersonaChange signal
  // the homepage reveal code listens for. Load does NOT go through here, so every
  // dispatch means "the user switched" — no skip-the-first-event gate is needed.
  function switchPersona(persona) {
    var next = persona === 'developers' ? 'developers' : 'people';
    if (next === currentPersona()) return; // already active: no rebuild, no replay
    applyPersona(next, true);
    rebuild(next);
    window.dispatchEvent(new CustomEvent('lemonadePersonaChange', { detail: { persona: next } }));
  }
  window.lemonadeSetPersona = switchPersona; // preserve the public hook

  function animationMode(slide) {
    if (slide && slide.animationMode) return slide.animationMode;
    return 'once';
  }

  function terminalCodeHtml(lines) {
    return lines.map(function(line) {
      var item = typeof line === 'string' ? { text: line } : line;
      var text = item.text || '';
      var classes = ['hp-terminal-line'];
      var style = '';
      if (/^\s*#/.test(text)) classes.push('hp-terminal-comment');
      if (item.kind) classes.push('hp-terminal-' + item.kind);
      if (typeof item.phase === 'number') classes.push('hp-terminal-phase-' + item.phase);
      if (typeof item.delay === 'number') style = ' style="--terminal-delay:' + item.delay + 'ms"';
      return '<span class="' + classes.join(' ') + '"' + style + '>' + escapeText(text || ' ') + '</span>';
    }).join('');
  }

  function commandDemo(kind) {
    var demos = {
      'terminal-dev-customize': {
        title: 'Bash',
        lines: [
          { text: '# Tune the runtime live', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ curl -XPOST :8123/internal/set \\', kind: 'command', phase: 0, delay: 470 },
          { text: '    -d \'{"ctx_size": 8192, "max_loaded_models": 3}\'', kind: 'command', phase: 0, delay: 720 },
          { text: '✓ settings applied', kind: 'output', phase: 0, delay: 1040 },
          { text: '', delay: 1340 },
          { text: '# ...or bake defaults into config.json', kind: 'comment', phase: 1, delay: 1640 },
          { text: '{ "ctx_size": 8192, "log_level": "info" }', kind: 'command', phase: 1, delay: 1960 }
        ]
      },
      'terminal-models-import': {
        title: 'Bash',
        lines: [
          { text: '# Point Lemonade at GGUF models already on your PC', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ lemonade config set extra_models_dir="/path/to/models"', kind: 'command', phase: 0, delay: 470 },
          { text: '✓ configuration updated', kind: 'output', phase: 0, delay: 820 },
          { text: '', delay: 1120 },
          { text: '$ lemonade list | grep custom', kind: 'command', phase: 1, delay: 1420 },
          { text: '✓ imported 3 GGUF models from /path/to/models', kind: 'output', phase: 1, delay: 1820 }
        ]
      },
      'terminal-selfhost': {
        title: 'Bash',
        lines: [
          { text: '# Protect your server with an API key', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ export LEMONADE_API_KEY="sk-lemon-••••••••"', kind: 'command', phase: 0, delay: 470 },
          { text: '✓ authentication enabled', kind: 'output', phase: 0, delay: 760 },
          { text: '', delay: 1040 },
          { text: '# Make Lemonade reachable across your LAN', kind: 'comment', phase: 1, delay: 1320 },
          { text: '$ lemonade config set host=0.0.0.0', kind: 'command', phase: 1, delay: 1640 },
          { text: '✓ now serving at http://192.168.1.42:8000', kind: 'output', phase: 1, delay: 1960 }
        ]
      },
      'terminal-cli-chat': {
        title: 'Bash',
        lines: [
          { text: '$ lemonade chat Qwen3.5-4B-GGUF', kind: 'command', phase: 0, delay: 160 },
          { text: '─── Qwen3.5-4B-GGUF ───  ? /help for shortcuts', kind: 'output', phase: 0, delay: 640 },
          { text: '', delay: 940 },
          { text: '> write a haiku about lemons', kind: 'command', phase: 1, delay: 1240 },
          { text: 'Bright yellow teardrops', kind: 'output', phase: 1, delay: 1620 },
          { text: 'hang from the summer branches—', kind: 'output', phase: 1, delay: 1820 },
          { text: 'tart sunshine in your hand.', kind: 'output', phase: 1, delay: 2020 },
          { text: '> /exit', kind: 'command', phase: 2, delay: 2420 }
        ]
      },
      'terminal-cli-backends': {
        title: 'Bash',
        lines: [
          { text: '$ lemonade backends', kind: 'command', phase: 0, delay: 160 },
          { text: 'RECIPE       ENGINE             DEVICE', kind: 'output', phase: 0, delay: 560 },
          { text: 'llamacpp     Vulkan · ROCm      GPU / CPU', kind: 'output', phase: 0, delay: 800 },
          { text: 'flm          FastFlowLM         NPU', kind: 'output', phase: 0, delay: 1000 },
          { text: 'ryzenai      Hybrid             NPU', kind: 'output', phase: 0, delay: 1200 },
          { text: 'whispercpp   whisper.cpp        CPU', kind: 'output', phase: 0, delay: 1400 },
          { text: '✓ 4 backends available', kind: 'output', phase: 1, delay: 1760 }
        ]
      },
      'terminal-cli-config': {
        title: 'Bash',
        lines: [
          { text: '# Tune the server from the command line', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ lemonade config set llamacpp.backend=rocm port=8123', kind: 'command', phase: 0, delay: 520 },
          { text: '✓ llamacpp.backend = rocm', kind: 'output', phase: 0, delay: 900 },
          { text: '✓ port = 8123', kind: 'output', phase: 0, delay: 1120 }
        ]
      },
      'terminal-api-openai': {
        title: 'Bash',
        lines: [
          { text: '# Chat · image · speech over the OpenAI API', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ curl :8000/v1/chat/completions -d \'{"messages":[{"role":"user","content":"Population of Paris?"}]}\'', kind: 'command', phase: 0, delay: 480 },
          { text: '{ "content": "Paris has about 2.2 million residents." }', kind: 'output', phase: 0, delay: 820 },
          { text: '', delay: 1120 },
          { text: '$ curl :8000/v1/images/generations -d \'{"model":"SD-Turbo","prompt":"a lemon grove"}\'', kind: 'command', phase: 1, delay: 1420 },
          { text: '✓ generated 512×512 PNG', kind: 'output', phase: 1, delay: 1740 },
          { text: '', delay: 2000 },
          { text: '$ curl :8000/v1/audio/speech -d \'{"model":"kokoro-v1","input":"Lemonade can speak!"}\'', kind: 'command', phase: 2, delay: 2300 },
          { text: '✓ speech.mp3 (1.2s)', kind: 'output', phase: 2, delay: 2620 }
        ]
      },
      'terminal-api-lemonade': {
        title: 'Bash',
        lines: [
          { text: '# Manage models, backends, and configuration', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ curl :8000/api/v1/pull -d \'{"model_name":"Qwen3-4B-GGUF"}\'', kind: 'command', phase: 0, delay: 480 },
          { text: '✓ installed Qwen3-4B-GGUF', kind: 'output', phase: 0, delay: 820 },
          { text: '', delay: 1100 },
          { text: '$ curl :8000/api/v1/install -d \'{"recipe":"llamacpp","backend":"vulkan"}\'', kind: 'command', phase: 1, delay: 1400 },
          { text: '✓ backend ready: llamacpp:vulkan', kind: 'output', phase: 1, delay: 1740 },
          { text: '', delay: 2000 },
          { text: '$ curl :8000/internal/set -d \'{"max_loaded_models":3}\'', kind: 'command', phase: 2, delay: 2300 },
          { text: '✓ max_loaded_models = 3', kind: 'output', phase: 2, delay: 2620 }
        ]
      },
      'terminal-api-ollama': {
        title: 'Bash',
        lines: [
          { text: '# Point any Ollama client at Lemonade', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ curl :11434/api/chat -d \'{"model":"Qwen3-4B-GGUF","messages":[{"role":"user","content":"Hello"}]}\'', kind: 'command', phase: 0, delay: 520 },
          { text: '{ "message": { "content": "Hello! How can I help you today?" } }', kind: 'output', phase: 0, delay: 900 },
          { text: '', delay: 1200 },
          { text: '$ curl :11434/api/tags', kind: 'command', phase: 1, delay: 1500 },
          { text: '{ "models": [ { "name": "Qwen3-4B-GGUF:latest" } ] }', kind: 'output', phase: 1, delay: 1840 }
        ]
      },
      'terminal-api-anthropic': {
        title: 'Bash',
        lines: [
          { text: '# The Anthropic Messages API, with tool use', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ curl :8000/api/messages -d \'{"max_tokens":100,"messages":[{"role":"user","content":"Say hello"}]}\'', kind: 'command', phase: 0, delay: 560 },
          { text: '{ "content": [ { "type": "text", "text": "Hello! I am here to help." } ] }', kind: 'output', phase: 0, delay: 1000 },
          { text: '', delay: 1300 },
          { text: '# tool use + SSE streaming supported', kind: 'comment', phase: 1, delay: 1600 }
        ]
      },
      'terminal-customize-runtime': {
        title: 'Bash',
        lines: [
          { text: '# Tune the runtime for your app', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ lemonade config set max_loaded_models=3 ctx_size=8192 llamacpp.backend=rocm', kind: 'command', phase: 0, delay: 520 },
          { text: '✓ max_loaded_models = 3', kind: 'output', phase: 0, delay: 880 },
          { text: '✓ ctx_size = 8192', kind: 'output', phase: 0, delay: 1060 },
          { text: '✓ llamacpp.backend = rocm', kind: 'output', phase: 0, delay: 1240 },
          { text: '', delay: 1520 },
          { text: '# ...or live, with no restart', kind: 'comment', phase: 1, delay: 1800 },
          { text: '$ curl :8000/internal/set -d \'{"ctx_size":16384}\'', kind: 'command', phase: 1, delay: 2120 },
          { text: '✓ applied', kind: 'output', phase: 1, delay: 2420 }
        ]
      },
      'terminal-customize-bundle': {
        title: 'Bash',
        lines: [
          { text: '# Bundle a private, branded deployment', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ lemonade config set models_dir="./models"', kind: 'command', phase: 0, delay: 480 },
          { text: '$ lemonade backends install llamacpp:vulkan', kind: 'command', phase: 0, delay: 760 },
          { text: '✓ bundled into your app folder', kind: 'output', phase: 0, delay: 1080 },
          { text: '', delay: 1360 },
          { text: '# launch hidden, locked to your app', kind: 'comment', phase: 1, delay: 1660 },
          { text: '$ LEMONADE_API_KEY=app-secret lemond ./ --port 8000', kind: 'command', phase: 1, delay: 1980 },
          { text: '✓ lemond running (private)', kind: 'output', phase: 1, delay: 2300 }
        ]
      }
    };
    var demo = demos[kind] || demos['terminal-dev-customize'];
    // The terminal is just the unified app window (dark theme) with a code body,
    // so it shares the chrome -- title bar + window dots on the right.
    return appWindow(demo.title || 'Bash',
      '<div class="hp-terminal-body"><pre><code>' + terminalCodeHtml(demo.lines) + '</code></pre></div>',
      'is-dark');
  }

  // User-persona chatbot window. An abstracted multimodal chatbot app: a light
  // frosted ice-card window (shared .hp-app-window chrome with the dev spawn
  // demo) titled "Lemonade", a conversation body, and a text-entry box whose
  // typed prompt animates being "sent" into the chat. The response per slide is
  // text / image / code / speech. Reuses the shared .hp-demo-chat/.hp-chat-*
  // card classes from website-styles.css; the stack autoplay drives rotation.
  function exploreDemo(kind) {
    var waveBars = new Array(26).join('<span></span>'); // 25 bars, matching the homepage hero waveform
    var cards = {
      'explore-chat': {
        prompt: 'What can I do with 128 GB of unified RAM?',
        body: '<div class="hp-demo-chat">' +
          '<div class="hp-chat-user">What can I do with 128 GB of unified RAM?</div>' +
          '<div class="hp-chat-ai">Load up models like gpt-oss-120b or Qwen-Coder-Next for advanced tool use.</div>' +
          '<div class="hp-chat-user">What should I tune first?</div>' +
          '<div class="hp-chat-ai">You can increase context size to 64k or more.</div>' +
        '</div>'
      },
      'explore-images': {
        prompt: 'A pitcher of lemonade in the style of a renaissance painting',
        body: '<div class="hp-demo-image">' +
          '<div class="hp-chat-user hp-demo-prompt">A pitcher of lemonade in the style of a renaissance painting</div>' +
          '<div class="hp-demo-image-placeholder"></div>' +
        '</div>'
      },
      'explore-coding': {
        prompt: 'Build a real-time dashboard that streams GPU metrics over WebSockets',
        body: '<div class="hp-demo-coding">' +
          '<div class="hp-chat-user">Build a real-time dashboard that streams GPU metrics over WebSockets</div>' +
          '<pre class="hp-code-block"><code>' +
            '<span class="hp-code-kw">async def</span> <span class="hp-code-fn">stream_gpu_metrics</span>(ws):\n' +
            '    <span class="hp-code-kw">while</span> <span class="hp-code-lit">True</span>:\n' +
            '        stats = <span class="hp-code-kw">await</span> gpu.poll()\n' +
            '        <span class="hp-code-kw">await</span> ws.send_json(stats)\n' +
            '        <span class="hp-code-kw">await</span> asyncio.sleep(<span class="hp-code-lit">0.5</span>)\n' +
            '<span class="hp-code-ellipsis">...</span>' +
          '</code></pre>' +
        '</div>'
      },
      'explore-speech': {
        prompt: 'Hello, I am your AI assistant. What can I do for you today?',
        body: '<div class="hp-demo-audio">' +
          '<div class="hp-chat-user hp-demo-prompt">Hello, I am your AI assistant. What can I do for you today?</div>' +
          '<div class="hp-waveform">' + waveBars + '</div>' +
        '</div>'
      }
    };
    var card = cards[kind] || cards['explore-chat'];
    return '<div class="hp-app-window ice-card hp-chatbot">' +
      '<div class="hp-app-window-bar">' +
        '<span class="hp-app-window-title">Lemonade</span>' +
        '<span class="hp-app-window-dots"><i></i><i></i><i></i></span>' +
      '</div>' +
      '<div class="hp-chatbot-body">' + card.body + '</div>' +
      '<div class="hp-chatbot-input">' +
        '<div class="hp-chatbot-field"><span class="hp-chatbot-typed">' + escapeText(card.prompt) + '</span><span class="hp-chatbot-caret"></span></div>' +
        '<span class="hp-chatbot-send"><span class="material-symbols-outlined">arrow_upward</span></span>' +
      '</div>' +
    '</div>';
  }

  // Converged "Explore" demo: ONE chatbot window whose transcript plays out every
  // modality in a single conversation -- chat, then image gen, then coding, then
  // speech. Same .hp-chatbot chrome as exploreDemo; the difference is the body holds
  // every turn at once and playOmni() reveals them in sequence, scrolling the feed up
  // so the newest turn stays in view (a real growing-chat feel). One-shot on render.
  function omniDemo() {
    var waveBars = new Array(26).join('<span></span>'); // 25 bars, matching the hero waveform
    var turns = [
      '<div class="hp-chat-user">What can I do with 128 GB of unified RAM?</div>',
      '<div class="hp-chat-ai">Load up models like gpt-oss-120b or Qwen-Coder-Next for advanced tool use.</div>',
      '<div class="hp-chat-user">Now paint a pitcher of lemonade like a renaissance master</div>',
      '<div class="hp-demo-image-placeholder"></div>',
      '<div class="hp-chat-user">Build a real-time dashboard that streams GPU metrics over WebSockets</div>',
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
    return '<div class="hp-app-window ice-card hp-chatbot hp-omni">' +
      '<div class="hp-app-window-bar">' +
        '<span class="hp-app-window-title">Lemonade</span>' +
        '<span class="hp-app-window-dots"><i></i><i></i><i></i></span>' +
      '</div>' +
      '<div class="hp-chatbot-body">' + feed + '</div>' +
      '<div class="hp-chatbot-input">' +
        '<div class="hp-chatbot-field"><span class="hp-omni-placeholder">Ask anything — text, images, code, or speech…</span></div>' +
        '<span class="hp-chatbot-send"><span class="material-symbols-outlined">arrow_upward</span></span>' +
      '</div>' +
    '</div>';
  }

  // Drive the omni transcript: reveal each turn on a timer, then translate the feed
  // up just enough to keep the freshly revealed turn in view. Measures real layout
  // (offsetTop/offsetHeight) so it stays correct regardless of bubble wrapping. Runs
  // once per render; honours reduced-motion by showing every turn statically.
  function playOmni(frameEl) {
    var body = frameEl.querySelector('.hp-chatbot-body');
    var feed = frameEl.querySelector('.hp-omni-feed');
    if (!body || !feed) return;
    var turns = feed.querySelectorAll('.hp-omni-turn');
    if (!turns.length) return;
    var reduce = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    if (reduce) {
      for (var r = 0; r < turns.length; r++) turns[r].classList.add('is-in');
      return;
    }
    // ms to dwell before the NEXT turn arrives (longer after image/code responses).
    var dwell = [700, 1150, 1300, 1550, 1300, 1650, 1300, 1500];
    var t = 450;
    for (var i = 0; i < turns.length; i++) {
      (function(turn) {
        window.setTimeout(function() {
          turn.classList.add('is-in');
          var overflow = turn.offsetTop + turn.offsetHeight - body.clientHeight + 14;
          feed.style.transform = 'translateY(' + (overflow > 0 ? -overflow : 0) + 'px)';
        }, t);
      })(turns[i]);
      t += dwell[i] || 1200;
    }
  }

  // A stylized mouse pointer that flies in and "clicks" UI in the model demos.
  // Movement (translate / right-top) lives on .hp-cursor; the click "press"
  // scales the inner <svg> -- kept on separate elements so the two never fight
  // over `transform`. Timing is driven by CSS scoped to each demo.
  var CURSOR_SVG =
    '<span class="hp-cursor" aria-hidden="true"><svg viewBox="0 0 24 24">' +
      '<path d="M4 2 L4 18.5 L8.4 14.2 L11.3 20.6 L13.8 19.4 L10.9 13.2 L16.8 13.2 Z"></path>' +
    '</svg></span>';

  // Shared app-window chrome: light frosted ice-card window with a centered title
  // and right-side window dots (same chrome as the chatbot + dev spawn demos).
  function appWindow(title, bodyHtml, extraClass) {
    return '<div class="hp-app-window ice-card' + (extraClass ? ' ' + extraClass : '') + '">' +
      '<div class="hp-app-window-bar">' +
        '<span class="hp-app-window-title">' + escapeText(title) + '</span>' +
        '<span class="hp-app-window-dots"><i></i><i></i><i></i></span>' +
      '</div>' + bodyHtml +
    '</div>';
  }

  // REUSABLE: a vertical list of items inside an app window, each with a download
  // button; the item flagged `downloading` shows a progress bar instead. Reused
  // later for the backends list. opts = { title, items: [{name, meta,
  // downloading, progress}] }.
  function appWindowList(opts) {
    var rows = opts.items.map(function(item, i) {
      // Every row is identical (name, meta, fixed action slot with the download
      // button) so the downloading row looks like the rest. On the downloading
      // row the cursor clicks the button at --swap-at; it then swaps to the
      // progress bar, which fills linearly to 100% (no deceleration).
      var act = item.downloading
        ? '<span class="hp-applist-act is-downloading" style="--swap-at:' + (item.swapAt || 1300) + 'ms">' +
            '<span class="hp-applist-dl"><span class="material-symbols-outlined">download</span></span>' +
            '<span class="hp-applist-progress" style="--p:100%"><i></i></span>' +
            CURSOR_SVG +
          '</span>'
        : '<span class="hp-applist-act">' +
            '<span class="hp-applist-dl"><span class="material-symbols-outlined">download</span></span>' +
          '</span>';
      return '<div class="hp-applist-row" style="--row:' + i + '">' +
          '<span class="hp-applist-name">' + escapeText(item.name) + '</span>' +
          (item.meta ? '<span class="hp-applist-meta">' + escapeText(item.meta) + '</span>' : '') +
          act +
        '</div>';
    }).join('');
    return appWindow(opts.title, '<div class="hp-applist">' + rows + '</div>', 'hp-applist-window');
  }

  function modelsDemo(kind) {
    if (kind === 'models-hf-search') return modelSearch();
    // models-registry: curated snapshot of suggested/"hot" models from
    // src/cpp/resources/server_models.json, ordered newest-released first.
    return appWindowList({
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
            CURSOR_SVG +
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
    return appWindow('Model Manager', body, 'hp-modelsearch-window');
  }

  // REUSABLE: an app-store list inside a stylized Lemonade window -- a vertical
  // list of app cards with real marketplace logos, name, description, and a
  // stylized (no-op) action button. Reused across app categories. Data is a
  // curated snapshot of the Lemonade Marketplace (apps.json); ids are the logo
  // folder slugs. A `placeholder` card renders a "…" monogram tile.
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
    ],
    'apps-selfhost': [
      { id: 'open-webui', name: 'Open WebUI', desc: 'Feature-rich web interface for chatting with LLMs locally.' },
      { id: 'lemonade-mobile', name: 'Lemonade Mobile', desc: 'iOS and Android chat app for your self-hosted Lemonade server.' },
      { id: 'dream-server', name: 'Dream Server', desc: 'Private local AI server for chat, agents, RAG, and self-hosted apps.' }
    ]
  };

  function appStore(kind) {
    var apps = appCategories[kind] || appCategories['apps-coding'];
    var cards = apps.map(function(app, i) {
      // Marketplace-style card: a header (logo + name), the description, then a
      // CTA row (stylized, no real action). Mirrors the layout of the website
      // marketplace page so each card fills more of the window.
      return '<div class="hp-appstore-card" style="--card:' + i + '">' +
          '<div class="hp-appstore-head">' +
            '<img class="hp-appstore-logo" src="' + APP_LOGO + escapeText(app.id) + '/logo.png" alt="" loading="lazy" />' +
            '<span class="hp-appstore-name">' + escapeText(app.name) + '</span>' +
          '</div>' +
          '<span class="hp-appstore-desc">' + escapeText(app.desc) + '</span>' +
          '<div class="hp-appstore-cta">' +
            '<span class="hp-appstore-btn is-primary"><span class="material-symbols-outlined">open_in_new</span>Visit</span>' +
            '<span class="hp-appstore-btn"><span class="material-symbols-outlined">menu_book</span>Guide</span>' +
          '</div>' +
        '</div>';
    }).join('');
    var body = '<div class="hp-appstore">' + cards + '<div class="hp-appstore-scrollhint"></div></div>';
    return appWindow('Lemonade', body, 'hp-appstore-window');
  }

  // "Connect to apps" overview: all nine featured apps on a single LIGHT stage (no
  // window chrome), grouped by category. Mirrors the dev-persona backend board, but
  // light. Cards are logo + name + description only -- no CTA buttons. Data is the
  // same curated marketplace snapshot as appStore (the three non-self-host groups).
  function appBoard() {
    var groups = [
      { label: 'Coding agents', kind: 'apps-coding' },
      { label: 'Personal agents', kind: 'apps-personal' },
      { label: 'Productivity', kind: 'apps-productivity' }
    ];
    var idx = 0;
    var html = groups.map(function(g) {
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
      return '<div class="hp-appboard-grouplabel">' + escapeText(g.label) + '</div>' +
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
          '<span class="hp-mcp-tool-name">' + escapeText(t.name) + '</span>' +
          '<span class="hp-mcp-tool-tag">' + escapeText(t.tag) + '</span>' +
        '</div>';
    }).join('');
    var config =
      '<pre class="hp-mcp-config"><code>' +
        '{\n' +
        '  <span class="hp-mcp-key">"mcpServers"</span>: {\n' +
        '<span class="hp-mcp-added">    <span class="hp-mcp-key">"lemonade"</span>: {\n' +
        '      <span class="hp-mcp-key">"url"</span>: <span class="hp-mcp-str">"http://localhost:13305/mcp"</span>\n' +
        '    }</span>\n' +
        '  }\n' +
        '}' +
      '</code></pre>';
    var body =
      '<div class="hp-mcp">' +
        '<div class="hp-mcp-filebar"><span class="material-symbols-outlined">description</span>mcp.json</div>' +
        config +
        '<div class="hp-mcp-status"><span class="hp-mcp-dot"></span>Lemonade connected — 5 tools available</div>' +
        '<div class="hp-mcp-tools">' + toolRows + '</div>' +
      '</div>';
    return appWindow('MCP Servers', body, 'hp-mcp-window');
  }

  // Developer "Private & white-labeled": an app window branded as the APP (not
  // Lemonade) wrapping a private, locked-down lemond — a bundled model library, an
  // API-key chip, and a footer noting lemond runs hidden. Dark variant to match the
  // developer demos. (Real basis: models_dir, LEMONADE_API_KEY, headless binary.)
  function privateApp() {
    var models = [
      { name: 'assistant-7b', tag: 'chat' },
      { name: 'vision-4b', tag: 'vision' },
      { name: 'voice-tiny', tag: 'speech' }
    ];
    var rows = models.map(function(m, i) {
      return '<div class="hp-private-row" style="--row:' + i + '">' +
          '<span class="hp-private-name">' + escapeText(m.name) + '</span>' +
          '<span class="hp-private-tag">' + escapeText(m.tag) + '</span>' +
          '<span class="hp-private-lock"><span class="material-symbols-outlined">lock</span></span>' +
        '</div>';
    }).join('');
    var body =
      '<div class="hp-private">' +
        '<div class="hp-private-keychip"><span class="material-symbols-outlined">key</span>Authorization: Bearer ••••••••</div>' +
        '<div class="hp-private-libhead">Private model library</div>' +
        '<div class="hp-private-lib">' + rows + '</div>' +
        '<div class="hp-private-foot"><span class="material-symbols-outlined">visibility_off</span>lemond runs hidden — your brand only</div>' +
      '</div>';
    return appWindow('YourApp', body, 'hp-private-window is-dark');
  }

  // Developer "Backends and devices": a grid of glass icon-cards showing the breadth
  // of engines / device acceleration. Dark variant. Material icons (no external logos).
  function backendBoard(kind) {
    var boards = {
      'backend-engines': {
        title: 'Inference engines',
        groups: [
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
        ]
      },
      'backend-devices': {
        title: 'Devices & acceleration',
        groups: [
          { items: [
            { icon: 'view_in_ar', name: 'Vulkan', tag: 'any GPU' },
            { icon: 'memory', name: 'ROCm', tag: 'AMD' },
            { icon: 'memory', name: 'CUDA', tag: 'NVIDIA' },
            { icon: 'laptop_mac', name: 'Metal', tag: 'Apple' },
            { icon: 'developer_board', name: 'CPU', tag: 'x86 · ARM' },
            { icon: 'bolt', name: 'NPU', tag: 'Ryzen AI' }
          ] }
        ]
      }
    };
    var board = boards[kind] || boards['backend-engines'];
    var idx = 0;
    var html = board.groups.map(function(g) {
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
    // On the stage (no window chrome) -- the board itself is the dark surface.
    return '<div class="hp-backend-board">' + html + '</div>';
  }

  // Render one slide's demo into a given section frame + caption.
  function renderDemo(frameEl, captionEl, step, slideIndex) {
    var slide = step.slides && step.slides[slideIndex];
    var demoKind = (slide && slide.demo) || step.demo;
    var captionText = slide && Object.prototype.hasOwnProperty.call(slide, 'caption') ? slide.caption : (step.copy || '');
    var captionHref = slide && slide.captionHref;
    var mode = animationMode(slide);
    if (captionEl) {
      if (captionText && captionHref) {
        captionEl.innerHTML = '<a class="hp-demo-caption-link" href="' + escapeText(captionHref) + '" target="_blank" rel="noopener">' +
          escapeText(captionText) + ' <span class="hp-demo-caption-arrow" aria-hidden="true">&#8594;</span></a>';
      } else {
        captionEl.textContent = captionText;
      }
      captionEl.hidden = !captionText;
    }
    frameEl.setAttribute('data-animation-mode', mode);
    if (demoKind.indexOf('router-') === 0 || demoKind === 'spawn-app' || demoKind === 'deploy-everywhere') {
      // Flowchart diagrams come from the flowchart.js module; hand it our cadence.
      frameEl.innerHTML = window.LemonadeFlowchart.render(demoKind, {
        subsectionDelay: animationSubsectionDelay,
        subsectionGap: animationSubsectionGap,
        minCycle: defaultAutoplayDelay
      });
    } else if (demoKind === 'explore-omni') {
      frameEl.innerHTML = omniDemo();
      playOmni(frameEl);
    } else if (demoKind.indexOf('explore-') === 0) {
      frameEl.innerHTML = exploreDemo(demoKind);
    } else if (demoKind.indexOf('models-') === 0) {
      frameEl.innerHTML = modelsDemo(demoKind);
    } else if (demoKind === 'apps-board') {
      frameEl.innerHTML = appBoard();
    } else if (demoKind === 'apps-mcp') {
      frameEl.innerHTML = mcpDemo();
    } else if (demoKind.indexOf('apps-') === 0) {
      frameEl.innerHTML = appStore(demoKind);
    } else if (demoKind === 'private-app') {
      frameEl.innerHTML = privateApp();
    } else if (demoKind.indexOf('backend-') === 0) {
      frameEl.innerHTML = backendBoard(demoKind);
    } else {
      frameEl.innerHTML = commandDemo(demoKind);
    }
    startSvgAnimations(frameEl);
  }

  // WebKit/Safari does not begin SMIL timelines for SVG inserted via innerHTML
  // (the graphic renders, but nothing animates). Re-inserting each <svg> as a
  // fresh DOM node kicks the timeline; it also harmlessly restarts the animation
  // from 0 in engines that already started it (a no-op for non-SVG demos).
  function startSvgAnimations(container) {
    if (!container || !container.querySelectorAll) return;
    var svgs = container.querySelectorAll('svg');
    for (var i = 0; i < svgs.length; i++) {
      var svg = svgs[i];
      if (!svg.parentNode) continue;
      // Re-insert every SVG to kick the timeline. (Don't try to skip non-animated
      // SVGs via querySelector('animate,...') -- WebKit doesn't reliably match SVG
      // SMIL elements by type selector, so the guard wrongly skips the kick and
      // nothing animates in Safari.)
      var fresh = svg.cloneNode(true);
      svg.parentNode.replaceChild(fresh, svg);
      try { if (fresh.setCurrentTime) fresh.setCurrentTime(0); } catch (e) {}
    }
  }

  // ---- Flatten the persona's sections + slides into one ordered list --------
  function flattenSlides(data) {
    var out = [];
    for (var si = 0; si < data.steps.length; si++) {
      var step = data.steps[si];
      var slides = step.slides && step.slides.length ? step.slides : [{ label: step.title, demo: step.demo }];
      for (var sj = 0; sj < slides.length; sj++) {
        out.push({ section: si, slideIndex: sj, step: step, slide: slides[sj] });
      }
    }
    return out;
  }

  // ---- Journey: a sticky TOC sidebar + slides in normal document flow --------
  // Dead simple: the slides are stacked normally and scroll past like any page;
  // the TOC is a sticky sidebar pinned to a fixed spot on the left. One observer
  // renders each slide's demo as it enters the viewport (so it animates on the way
  // in); another highlights the TOC entry for whichever slide is centred. Clicking
  // a TOC entry scrolls to that slide. Every demo sits in a fixed-size box so the
  // slides are uniform and the TOC column never shifts.
  var globalSlides = [];
  var tocEl = null;
  var slideEls = null;
  var demoEls = null;
  var captionEls = null;
  var rendered = [];
  var currentActive = -1;
  var renderIO = null;
  var activeIO = null;

  function buildJourney(persona) {
    var data = personaSteps[persona] || personaSteps.people;
    if (zoneEl) zoneEl.textContent = data.zone || '';
    if (zoneSubtitleEl) zoneSubtitleEl.textContent = data.zoneSubtitle || '';
    globalSlides = flattenSlides(data);
    var dev = persona === 'developers';

    var gi = 0;
    var sectionsHtml = data.steps.map(function(step, si) {
      var slides = step.slides && step.slides.length ? step.slides : [{ label: step.title }];
      var sectionStart = gi;
      var slidesHtml = slides.map(function(slide) {
        var g = gi; gi += 1;
        return '<li class="hp-toc-slide-item">' +
            '<button class="hp-toc-slide" type="button" data-global="' + g + '">' +
              '<span class="hp-toc-dot" aria-hidden="true"></span>' +
              '<span class="hp-toc-slide-label">' + escapeText(slide.label) + '</span>' +
            '</button>' +
          '</li>';
      }).join('');
      var badge = dev
        ? '<span class="hp-toc-sec-badge">' + (si + 1) + '</span>'
        : '<span class="hp-toc-sec-badge"><span class="material-symbols-outlined">' + (STEP_ICONS[si] || 'circle') + '</span></span>';
      return '<li class="hp-toc-section" data-section="' + si + '">' +
          '<button class="hp-toc-section-btn" type="button" data-global="' + sectionStart + '">' +
            badge +
            '<span class="hp-toc-sec-title">' + escapeText(step.title) + '</span>' +
          '</button>' +
          '<ol class="hp-toc-slides">' + slidesHtml + '</ol>' +
        '</li>';
    }).join('');

    var slidesHtml = globalSlides.map(function(entry, i) {
      return '<section class="hp-slide" id="hp-slide-' + i + '" data-global="' + i + '">' +
          '<div class="hp-slide-demo"></div>' +
          '<p class="hp-demo-caption hp-slide-caption" hidden></p>' +
        '</section>';
    }).join('');

    journeyEl.innerHTML =
      '<nav class="hp-journey-toc" aria-label="' + escapeText(data.label) + '">' +
        '<ol class="hp-toc-list">' + sectionsHtml + '</ol>' +
      '</nav>' +
      '<div class="hp-journey-slides">' + slidesHtml + '</div>';

    tocEl = journeyEl.querySelector('.hp-journey-toc');
    slideEls = journeyEl.querySelectorAll('.hp-slide');
    demoEls = journeyEl.querySelectorAll('.hp-slide-demo');
    captionEls = journeyEl.querySelectorAll('.hp-slide-caption');
    rendered = [];
    currentActive = -1;
  }

  // Render a slide's demo + caption once, as it enters the viewport.
  function renderSlide(i) {
    if (i < 0 || i >= demoEls.length || rendered[i]) return;
    rendered[i] = true;
    var entry = globalSlides[i];
    renderDemo(demoEls[i], captionEls[i], entry.step, entry.slideIndex);
  }

  function setActive(i) {
    if (i === currentActive) return;
    currentActive = i;
    updateToc(i);
  }

  function updateToc(g) {
    if (!tocEl) return;
    var entry = globalSlides[g];
    var secs = tocEl.querySelectorAll('.hp-toc-section');
    for (var s = 0; s < secs.length; s++) {
      secs[s].classList.toggle('is-current', Number(secs[s].getAttribute('data-section')) === entry.section);
    }
    var slides = tocEl.querySelectorAll('.hp-toc-slide');
    for (var k = 0; k < slides.length; k++) {
      var on = Number(slides[k].getAttribute('data-global')) === g;
      slides[k].classList.toggle('is-active', on);
      if (on) slides[k].setAttribute('aria-current', 'true'); else slides[k].removeAttribute('aria-current');
    }
  }

  function setupJourney() {
    // Render each slide's demo a little before it scrolls in, so it animates on
    // the way into view (normal page behaviour).
    renderIO = new IntersectionObserver(function(entries) {
      entries.forEach(function(e) {
        if (e.isIntersecting) renderSlide(Number(e.target.getAttribute('data-global')) || 0);
      });
    }, { rootMargin: '200px 0px 200px 0px', threshold: 0 });
    // Highlight the TOC for whichever slide is crossing the middle of the viewport.
    activeIO = new IntersectionObserver(function(entries) {
      entries.forEach(function(e) {
        if (e.isIntersecting) setActive(Number(e.target.getAttribute('data-global')) || 0);
      });
    }, { rootMargin: '-42% 0px -42% 0px', threshold: 0 });
    for (var i = 0; i < slideEls.length; i++) {
      renderIO.observe(slideEls[i]);
      activeIO.observe(slideEls[i]);
    }
  }

  function jumpToGlobal(g) {
    var el = document.getElementById('hp-slide-' + g);
    if (el) el.scrollIntoView({ behavior: 'smooth', block: 'center' });
  }

  function rebuild(persona) {
    if (renderIO) { renderIO.disconnect(); renderIO = null; }
    if (activeIO) { activeIO.disconnect(); activeIO = null; }
    buildJourney(persona);
    setupJourney();
    updateHeroPersonaSwitch(persona);
  }

  function updateHeroPersonaSwitch(persona) {
    var current = persona === 'developers' ? 'developers' : 'people';
    document.querySelectorAll('[data-persona-choice]').forEach(function(btn) {
      btn.setAttribute('aria-pressed', btn.getAttribute('data-persona-choice') === current ? 'true' : 'false');
    });
  }

  journeyEl.addEventListener('click', function(event) {
    var btn = event.target.closest && event.target.closest('[data-global]');
    if (!btn) return;
    event.preventDefault();
    jumpToGlobal(Number(btn.getAttribute('data-global')) || 0);
  });

  // Hero CTA buttons (persona-aware labels toggled in CSS): the primary button
  // smooth-scrolls down to that persona's Quick Start section (its id is named in
  // data-cta-scroll); the secondary switches persona in place (no scroll) via
  // switchPersona, which owns the side effects (dark scheme, persistence, signal).
  document.addEventListener('click', function(event) {
    if (!event.target.closest) return;
    var scrollBtn = event.target.closest('[data-cta-scroll]');
    if (scrollBtn) {
      var target = document.getElementById(scrollBtn.getAttribute('data-cta-scroll'));
      if (target && target.scrollIntoView) target.scrollIntoView({ behavior: 'smooth', block: 'start' });
      return;
    }
    var switchBtn = event.target.closest('[data-cta-switch]');
    if (switchBtn) switchPersona(switchBtn.getAttribute('data-cta-switch'));
  });

  // Apply the persona on load via the silent path (no signal, so no reveal replay),
  // then build the journey. The install Quick Start is user-persona content, so
  // landing directly on its anchor forces the user persona (else it is display:none
  // and the browser scrolls to nothing).
  function init() {
    if (window.location.hash === '#getting-started') applyPersona('people', true);
    else applyPersona(readStoredPersona(), false);
    rebuild(currentPersona());
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})();
