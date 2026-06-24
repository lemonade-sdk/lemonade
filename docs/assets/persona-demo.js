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
  var STEP_ICONS = ['explore', 'apps', 'developer_board', 'terminal', 'dns'];  // people steps
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
              label: 'Connect any OpenAI app',
              demo: 'apps-connect',
              caption: 'Point any OpenAI-compatible app at Lemonade — just set the base URL and connect.',
              captionHref: 'https://lemonade-server.ai/docs/api/openai/',
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
          eyebrow: 'Backends',
          title: 'Try the backends',
          copy: 'Download inference engines and benchmark them on your own hardware.',
          demo: 'backend-manager',
          slides: [
            {
              label: 'Install inference engines',
              demo: 'backend-manager',
              caption: 'Download the inference engines you want — FastFlowLM, llama.cpp, Ryzen AI, and vLLM.',
              captionHref: 'https://lemonade-server.ai/docs/embeddable/backends/',
              animationMode: 'once',
              duration: 7800
            },
            {
              label: 'Benchmark with one command',
              demo: 'terminal-bench',
              caption: 'Compare backends head-to-head on your own hardware with lemonade bench.',
              captionHref: 'https://lemonade-server.ai/docs/guide/cli/',
              animationMode: 'once',
              duration: 4600
            }
          ]
        },
        {
          eyebrow: 'CLI',
          title: 'Learn the CLI',
          copy: 'Everything Lemonade does is one command away.',
          demo: 'terminal-cli-launch',
          slides: [
            {
              label: 'Launch a coding agent',
              demo: 'terminal-cli-launch',
              caption: 'Run Claude Code, Codex, opencode, or pi on your own model — private, and free.',
              captionHref: 'https://lemonade-server.ai/docs/guide/cli/#options-for-launch',
              animationMode: 'once',
              duration: 5400
            },
            {
              label: 'Chat REPL',
              demo: 'terminal-cli-chat',
              caption: 'Chat with any model right in your terminal.',
              captionHref: 'https://lemonade-server.ai/docs/guide/cli-chat/',
              animationMode: 'once',
              duration: 3800
            },
            {
              label: 'Manage your models',
              demo: 'terminal-cli-library',
              caption: 'Browse, pull, and load any model straight from the shell.',
              captionHref: 'https://lemonade-server.ai/docs/guide/cli/#options-for-pull',
              animationMode: 'once',
              duration: 4200
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
              demo: 'household-network',
              caption: 'Your secured server streams private AI to every device — Open WebUI, Dream Server, and the mobile app.',
              captionHref: 'https://lemonade-server.ai/docs/integrations/open-webui/',
              animationMode: 'repeat',
              duration: 5200
            }
          ]
        }
      ]
    },
    developers: {
      title: 'One router. Every backend. Any app.',
      subtitle: 'Embed lemond behind one clean OpenAI-compatible API and ship local AI across CPU, GPU, NPU, RAM, and cloud.',
      zone: 'Meet your new AI stack.',
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
              label: 'Own the whole stack',
              demo: 'private-app',
              caption: 'Bundle private models and backends, lock them to your API key, and keep every spotlight on your app.',
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
              label: 'Ollama & Anthropic API',
              demo: 'terminal-api-compat',
              caption: 'Bring any Ollama client — or Anthropic-API apps like Claude Code — to local models.',
              captionHref: 'https://lemonade-server.ai/docs/api/anthropic/',
              animationMode: 'once',
              duration: 4600
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
      'terminal-bench': {
        title: 'Bash',
        lines: [
          { text: '# Benchmark one model across backends', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ lemonade bench Qwen3.5-4B --backends llamacpp,vllm', kind: 'command', phase: 0, delay: 470 },
          { text: '', delay: 820 },
          { text: '  BACKEND      DEVICE       PROMPT t/s   DECODE t/s', kind: 'output', phase: 1, delay: 1180 },
          { text: '  ───────────────────────────────────────────────', kind: 'output', phase: 1, delay: 1380 },
          { text: '  llama.cpp    Vulkan        ████         ████', kind: 'output', phase: 1, delay: 1700 },
          { text: '  vLLM         ROCm          ████         ████', kind: 'output', phase: 2, delay: 2200 }
        ]
      },
      'terminal-dev-customize': {
        title: 'Bash',
        lines: [
          { text: '# Tune the runtime live', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ curl -XPOST :13305/internal/set \\', kind: 'command', phase: 0, delay: 470 },
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
          { text: '✓ now serving at http://192.168.1.42:13305', kind: 'output', phase: 1, delay: 1960 }
        ]
      },
      'terminal-cli-launch': {
        title: 'Bash',
        lines: [
          { text: '# Pick a model and launch the agent', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ lemonade launch pi', kind: 'command', phase: 0, delay: 470 },
          { text: '? Choose a model   ↑/↓ · ↵ to select', kind: 'output', phase: 0, delay: 820 },
          { text: '  ❯ Qwen3-Coder-30B-A3B-Instruct-GGUF', kind: 'output', phase: 0, delay: 1080 },
          { text: '    Devstral-Small-2507-GGUF', kind: 'output', phase: 0, delay: 1240 },
          { text: '    GLM-4.7-Flash-GGUF', kind: 'output', phase: 0, delay: 1400 },
          { text: '✓ pi is live  ·  100% local · $0 / token', kind: 'output', phase: 1, delay: 1820 },
          { text: '', delay: 2120 },
          { text: '  pi › build a CLI todo app with sqlite', kind: 'command', phase: 2, delay: 2420 },
          { text: '  ● wrote todo.py, db.py, test_todo.py', kind: 'output', phase: 2, delay: 2820 },
          { text: '  ● ran pytest   ✓ 8 passed', kind: 'output', phase: 2, delay: 3120 }
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
          { text: '', delay: 2220 },
          { text: '> /exit', kind: 'command', phase: 2, delay: 2520 }
        ]
      },
      'terminal-cli-library': {
        title: 'Bash',
        lines: [
          { text: '# Manage your whole model library — no GUI', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ lemonade pull \\', kind: 'command', phase: 0, delay: 470 },
          { text: '    Qwen3-Coder-30B-A3B-Instruct-GGUF', kind: 'command', phase: 0, delay: 720 },
          { text: '✓ downloaded · ready to load', kind: 'output', phase: 0, delay: 1040 },
          { text: '', delay: 1340 },
          { text: '$ lemonade list --downloaded', kind: 'command', phase: 1, delay: 1640 },
          { text: 'MODEL                       RECIPE', kind: 'output', phase: 1, delay: 1900 },
          { text: 'Gemma-4-E2B-it-GGUF         llamacpp', kind: 'output', phase: 1, delay: 2100 },
          { text: 'Qwen3-Coder-30B-A3B-GGUF    llamacpp', kind: 'output', phase: 1, delay: 2300 },
          { text: 'kokoro-v1                   kokoro', kind: 'output', phase: 1, delay: 2500 }
        ]
      },
      'terminal-api-openai': {
        title: 'Bash',
        lines: [
          { text: '# Chat · image · speech over the OpenAI API', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ curl :13305/v1/chat/completions -d \'{"messages":[{"role":"user","content":"Population of Paris?"}]}\'', kind: 'command', phase: 0, delay: 480 },
          { text: '{ "content": "Paris has about 2.2 million residents." }', kind: 'output', phase: 0, delay: 820 },
          { text: '', delay: 1120 },
          { text: '$ curl :13305/v1/images/generations -d \'{"model":"SD-Turbo","prompt":"a lemon grove"}\'', kind: 'command', phase: 1, delay: 1420 },
          { text: '✓ generated 512×512 PNG', kind: 'output', phase: 1, delay: 1740 },
          { text: '', delay: 2000 },
          { text: '$ curl :13305/v1/audio/speech -d \'{"model":"kokoro-v1","input":"Lemonade can speak!"}\'', kind: 'command', phase: 2, delay: 2300 },
          { text: '✓ speech.mp3 (1.2s)', kind: 'output', phase: 2, delay: 2620 }
        ]
      },
      'terminal-api-lemonade': {
        title: 'Bash',
        lines: [
          { text: '# Manage models, backends, and configuration', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ curl :13305/api/v1/pull -d \'{"model_name":"Qwen3-4B-GGUF"}\'', kind: 'command', phase: 0, delay: 480 },
          { text: '✓ installed Qwen3-4B-GGUF', kind: 'output', phase: 0, delay: 820 },
          { text: '', delay: 1100 },
          { text: '$ curl :13305/api/v1/install -d \'{"recipe":"llamacpp","backend":"vulkan"}\'', kind: 'command', phase: 1, delay: 1400 },
          { text: '✓ backend ready: llamacpp:vulkan', kind: 'output', phase: 1, delay: 1740 },
          { text: '', delay: 2000 },
          { text: '$ curl :13305/internal/set -d \'{"max_loaded_models":3}\'', kind: 'command', phase: 2, delay: 2300 },
          { text: '✓ max_loaded_models = 3', kind: 'output', phase: 2, delay: 2620 }
        ]
      },
      'terminal-api-compat': {
        title: 'Bash',
        lines: [
          { text: '# Your Ollama tools, pointed at Lemonade — unchanged', kind: 'comment', phase: 0, delay: 160 },
          { text: '$ OLLAMA_HOST=localhost:13305 ollama run qwen3', kind: 'command', phase: 0, delay: 520 },
          { text: '>>> Hello! Running 100% local on your GPU.', kind: 'output', phase: 0, delay: 900 },
          { text: '', delay: 1200 },
          { text: '# Anthropic API — point Claude Code at local models', kind: 'comment', phase: 1, delay: 1600 },
          { text: '$ ANTHROPIC_BASE_URL=http://localhost:13305 claude', kind: 'command', phase: 1, delay: 2000 },
          { text: '● Qwen3-Coder · 100% local · $0 / token', kind: 'output', phase: 1, delay: 2380 },
          { text: '● edited app.py · ran tests ✓ 8 passed', kind: 'output', phase: 2, delay: 2820 }
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
          { text: '$ curl :13305/internal/set -d \'{"ctx_size":16384}\'', kind: 'command', phase: 1, delay: 2120 },
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
          { text: '$ LEMONADE_API_KEY=app-secret lemond ./ --port 13305', kind: 'command', phase: 1, delay: 1980 },
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
    var category = opts.category
      ? '<div class="hp-applist-category">' + escapeText(opts.category) + '</div>'
      : '';
    return appWindow(opts.title, '<div class="hp-applist">' + category + rows + '</div>', 'hp-applist-window');
  }

  // "Try the backends": a backend manager mirroring the model manager -- a "Large
  // Language Models" category, then the four LLM inference engines to download. All
  // four are flagged downloading with staggered --swap-at, so the single cursor
  // (one visible at a time) clicks each download button top-to-bottom in sequence.
  function backendManager() {
    return appWindowList({
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

  // One shared cursor that glides smoothly between the download buttons of an
  // appWindowList (model manager + backend manager), pressing each in --swap-at
  // order. Replaces the old per-row fly-in cursors so it reads as a single mouse
  // moving down the list. Measures live button positions, so it works for either
  // demo (all rows, or just a couple). One-shot; skipped under reduced motion.
  function playDownloadCursor(frameEl) {
    var list = frameEl.querySelector('.hp-applist');
    if (!list) return;
    var acts = list.querySelectorAll('.hp-applist-act.is-downloading');
    if (!acts.length) return;
    if (window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches) return;

    var holder = document.createElement('div');
    holder.innerHTML = CURSOR_SVG;
    var cursor = holder.firstChild;
    cursor.classList.add('hp-applist-cursor');
    list.appendChild(cursor);

    var TIP_X = 4, TIP_Y = 3;   // the pointer tip sits near the svg's top-left
    function center(act) {
      var dl = act.querySelector('.hp-applist-dl') || act;
      var lr = list.getBoundingClientRect();
      var r = dl.getBoundingClientRect();
      return { x: r.left - lr.left + r.width / 2, y: r.top - lr.top + r.height / 2 };
    }
    function place(p) {
      cursor.style.transform = 'translate(' + (p.x - TIP_X) + 'px, ' + (p.y - TIP_Y) + 'px)';
    }
    function press() {
      var svg = cursor.querySelector('svg');
      if (!svg) return;
      svg.style.animation = 'none';
      void svg.offsetWidth;
      svg.style.animation = 'hp-cursor-press 0.22s ease';
    }

    var targets = [];
    for (var i = 0; i < acts.length; i++) {
      var sa = parseFloat(String(acts[i].style.getPropertyValue('--swap-at')).replace('ms', '')) || 1300;
      targets.push({ act: acts[i], swapAt: sa });
    }
    targets.sort(function(a, b) { return a.swapAt - b.swapAt; });

    var glideLead = 550;   // start moving to the next button this long before its click

    // Fade in just up-left of the first button, then glide onto it.
    window.setTimeout(function() {
      var first = center(targets[0].act);
      cursor.style.transition = 'none';
      place({ x: first.x - 26, y: first.y - 22 });
      void cursor.offsetWidth;
      cursor.style.transition = '';
      cursor.classList.add('is-in');
      place(first);
    }, Math.max(0, targets[0].swapAt - 600));

    targets.forEach(function(t, idx) {
      window.setTimeout(function() { place(center(t.act)); press(); }, t.swapAt);
      var next = targets[idx + 1];
      if (next) {
        window.setTimeout(function() { place(center(next.act)); }, next.swapAt - glideLead);
      }
    });
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
    return appWindow('MCP Servers', body, 'hp-mcp-window');
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
                CURSOR_SVG +
              '</button>' +
              '<div class="hp-conn-status">' +
                '<span class="hp-conn-dot"></span>' +
                '<span class="hp-conn-status-text">100+ models found!</span>' +
              '</div>' +
            '</div>' +
          '</div>' +
        '</div>' +
      '</div>';
    return appWindow('Any OpenAI Compatible App', body, 'hp-conn-window');
  }

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
    return appWindow('Your App', body, 'hp-private-window is-dark');
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

  // Render one slide's demo into a given section frame + caption. When animate is
  // false the demo is built in its START state -- JS players are not run and CSS
  // animations are frozen by the .hp-slide:not(.is-active) rule -- so an upcoming
  // (not-yet-reached) slide shows its "before" look, never its finished frame.
  // refreshActive replays with animate=true once the slide reaches the live zone.
  function renderDemo(frameEl, captionEl, step, slideIndex, animate) {
    if (animate === undefined) animate = true;
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
    if (demoKind.indexOf('router-') === 0 || demoKind === 'spawn-app' || demoKind === 'deploy-everywhere' || demoKind === 'household-network') {
      // Flowchart diagrams come from the flowchart.js module; hand it our cadence.
      frameEl.innerHTML = window.LemonadeFlowchart.render(demoKind, {
        subsectionDelay: animationSubsectionDelay,
        subsectionGap: animationSubsectionGap,
        minCycle: defaultAutoplayDelay
      });
    } else if (demoKind === 'explore-omni') {
      frameEl.innerHTML = omniDemo();
      if (animate) playOmni(frameEl);
    } else if (demoKind.indexOf('explore-') === 0) {
      frameEl.innerHTML = exploreDemo(demoKind);
    } else if (demoKind.indexOf('models-') === 0) {
      frameEl.innerHTML = modelsDemo(demoKind);
      if (animate) playDownloadCursor(frameEl);
    } else if (demoKind === 'apps-board') {
      frameEl.innerHTML = appBoard();
    } else if (demoKind === 'apps-connect') {
      frameEl.innerHTML = connectDemo();
    } else if (demoKind === 'apps-mcp') {
      frameEl.innerHTML = mcpDemo();
    } else if (demoKind.indexOf('apps-') === 0) {
      frameEl.innerHTML = appStore(demoKind);
    } else if (demoKind === 'private-app') {
      frameEl.innerHTML = privateApp();
    } else if (demoKind === 'backend-manager') {
      frameEl.innerHTML = backendManager();
      if (animate) playDownloadCursor(frameEl);
    } else if (demoKind.indexOf('backend-') === 0) {
      frameEl.innerHTML = backendBoard(demoKind);
    } else {
      frameEl.innerHTML = commandDemo(demoKind);
    }
    if (animate) startSvgAnimations(frameEl);
    else freezeSvgAnimations(frameEl);
  }

  // Freeze SVG (SMIL) timelines at their start -- used for pre-rendered upcoming
  // slides so a flowchart doesn't auto-play before it reaches the live zone.
  function freezeSvgAnimations(container) {
    if (!container || !container.querySelectorAll) return;
    var svgs = container.querySelectorAll('svg');
    for (var i = 0; i < svgs.length; i++) {
      try {
        if (svgs[i].setCurrentTime) svgs[i].setCurrentTime(0);
        if (svgs[i].pauseAnimations) svgs[i].pauseAnimations();
      } catch (e) {}
    }
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
  var currentActive = -1;   // the highlighted (focused) slide
  var playedIndex = -1;     // the slide whose animation has played for this arrival
  var renderIO = null;

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
          '<div class="hp-slide-stage"><div class="hp-slide-demo"></div></div>' +
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
    playedIndex = -1;
    jumpIntent = -1;
  }

  // Pre-render a slide's demo in its START state as it nears the viewport, so an
  // upcoming neighbour shows its "before" look (not a finished animation) while it
  // sits dimmed in the depth-of-field. The animation is played later, by refreshActive.
  function renderSlide(i) {
    if (i < 0 || i >= demoEls.length || rendered[i]) return;
    rendered[i] = true;
    var entry = globalSlides[i];
    renderDemo(demoEls[i], captionEls[i], entry.step, entry.slideIndex, false);
  }

  // Re-render a slide's demo and PLAY it. Used when a slide reaches the live zone so
  // its animation runs THERE (from the start), not when it first scrolled into view.
  function replaySlide(i) {
    if (i < 0 || i >= demoEls.length) return;
    rendered[i] = true;
    var entry = globalSlides[i];
    renderDemo(demoEls[i], captionEls[i], entry.step, entry.slideIndex, true);
  }

  // Depth-of-field + TOC highlight ONLY (no replay). Tracking focus while scrolling
  // must not re-trigger animations -- replay is handled separately, gated to the
  // live zone (see refreshActive).
  function setHighlight(i) {
    if (i === currentActive) return;
    currentActive = i;
    for (var s = 0; s < slideEls.length; s++) {
      slideEls[s].classList.toggle('is-active', s === i);
    }
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

  // THE single source of truth for "which slide is focused": the one whose demo
  // centre is nearest the viewport centre. Symmetric by construction -- both the
  // highlight and the magnetic snap derive the focus from this one function, so they
  // can never disagree. (That disagreement was the bug: the old activation used an
  // asymmetric "last slide at/above centre" test while snap used nearest, so a slide
  // snapped to centre could land a sub-pixel below the line and the previous slide
  // stayed active.) Because snap centres nearestSlide() and the active slide IS
  // nearestSlide(), a centred slide is always the active slide.
  function nearestSlide() {
    var vc = window.innerHeight / 2;
    var best = -1, bestAbs = Infinity, bestDelta = 0;
    for (var i = 0; i < demoEls.length; i++) {
      var r = demoEls[i].getBoundingClientRect();
      var delta = (r.top + r.height / 2) - vc;
      var abs = delta < 0 ? -delta : delta;
      if (abs < bestAbs) { bestAbs = abs; bestDelta = delta; best = i; }
    }
    return { index: best, abs: bestAbs, delta: bestDelta };
  }

  // Is the journey "engaged" -- does it own the screen? True only while the viewport
  // centre lies within the journey's slide span (first slide's centre .. last
  // slide's centre), plus a small tolerance so the first/last slides engage as they
  // become substantially centred rather than at the exact pixel. This is the gate
  // that stops the first slide grabbing focus while it merely peeks up from the
  // bottom (you're still reading the section above): until its centre climbs to the
  // lower third, the journey stays dormant. Symmetric, so it also releases you when
  // you scroll out the bottom into the sections below.
  function journeyEngaged() {
    if (!demoEls || !demoEls.length) return false;
    var vc = window.innerHeight / 2;
    var m = window.innerHeight * 0.15;
    var f = demoEls[0].getBoundingClientRect();
    var l = demoEls[demoEls.length - 1].getBoundingClientRect();
    var firstCenter = f.top + f.height / 2;
    var lastCenter = l.top + l.height / 2;
    return firstCenter <= vc + m && lastCenter >= vc - m;
  }

  // Drop the spotlight when the journey isn't engaged, so no slide is left crisp
  // while you're above (or below) the journey.
  function clearActive() {
    playedIndex = -1;
    if (currentActive === -1) return;
    currentActive = -1;
    for (var s = 0; s < slideEls.length; s++) slideEls[s].classList.remove('is-active');
  }

  // React to the current scroll position: highlight the nearest slide, and once it
  // has settled into the live zone (near centre) play its animation -- once per
  // arrival. The replay is gated to the live zone (not the moment focus changes) so
  // animations still run while the slide is centred, where the user can see them.
  function refreshActive() {
    if (!demoEls || !demoEls.length) return;
    if (!journeyEngaged()) { clearActive(); return; }
    // While an explicit jump is in flight, the clicked slide owns the focus -- the
    // highlight must not flicker onto slides we merely scroll past on the way there.
    var index, abs;
    if (jumpIntent !== -1) {
      index = jumpIntent;
      var ri = demoEls[index].getBoundingClientRect();
      abs = Math.abs((ri.top + ri.height / 2) - window.innerHeight / 2);
    } else {
      var n = nearestSlide();
      if (n.index === -1) return;
      index = n.index;
      abs = n.abs;
    }
    setHighlight(index);
    var liveZone = Math.min(140, window.innerHeight * 0.14);
    if (abs <= liveZone) {
      if (playedIndex !== index) { playedIndex = index; replaySlide(index); }
    } else {
      playedIndex = -1;   // left the live zone -> arm replay for whatever centres next
    }
  }

  var scrollTicking = false;
  var snapTimer = null;
  var isSnapping = false;
  var snapTarget = -1;
  var jumpIntent = -1;   // global slide the user explicitly asked for; the magnet must serve it

  function onJourneyScroll() {
    if (!scrollTicking) {
      scrollTicking = true;
      window.requestAnimationFrame(function() {
        scrollTicking = false;
        refreshActive();
      });
    }
    // Magnetic centring: once scrolling settles, glide the nearest slide to centre.
    if (snapTimer) window.clearTimeout(snapTimer);
    snapTimer = window.setTimeout(snapToCenter, 140);
  }

  // After scrolling stops, glide the SAME slide refreshActive focuses on -- the
  // nearest -- so its demo sits dead-centre. Removes the need to land a slide on the
  // centre line by hand. Scoped to the journey (only fires when a demo is genuinely
  // near centre), skipped on mobile + under reduced motion, and self-limiting: it
  // no-ops once centred and ignores the smooth scroll it triggers (by only
  // re-snapping when the nearest slide actually changes).
  function snapToCenter() {
    if (!demoEls || !demoEls.length) return;
    if (window.matchMedia &&
        (window.matchMedia('(max-width: 920px)').matches ||
         window.matchMedia('(prefers-reduced-motion: reduce)').matches)) return;
    if (!journeyEngaged()) { isSnapping = false; snapTarget = -1; return; }   // don't grab on entry/exit
    // An explicit jump is authoritative: serve its target, never override it with
    // the nearest slide. If the smooth scroll stalled short, finish it; only when
    // the clicked slide is centred do we release the intent back to the magnet.
    if (jumpIntent !== -1) {
      var ri = demoEls[jumpIntent].getBoundingClientRect();
      var d = (ri.top + ri.height / 2) - window.innerHeight / 2;
      if (Math.abs(d) < 6) { jumpIntent = -1; isSnapping = false; snapTarget = -1; return; }
      window.scrollTo({ top: window.pageYOffset + d, behavior: 'smooth' });
      return;
    }
    var n = nearestSlide();
    if (n.index === -1) { isSnapping = false; snapTarget = -1; return; }
    if (n.abs < 6) { isSnapping = false; snapTarget = -1; return; }   // already centred
    if (isSnapping && n.index === snapTarget) return;                  // already gliding there
    isSnapping = true;
    snapTarget = n.index;
    window.scrollTo({ top: window.pageYOffset + n.delta, behavior: 'smooth' });
  }

  // Centre the sticky TOC by setting its sticky `top` to (viewport - tocHeight) / 2,
  // with NO transform. Centring via `top` (not transform) is what keeps it correct
  // across the whole page: sticky clamps the element to its containing block (the
  // grid, which starts below the zone heading), so the TOC can never render above
  // the heading -- during approach it sits just under the heading, and it pins
  // centred only once scrolled in. On mobile the TOC is a top bar, so clear the
  // inline top and let the stylesheet own it. If the outline is taller than the
  // viewport, top clamps to a small margin and the TOC scrolls.
  function positionToc() {
    if (!tocEl) return;
    if (window.matchMedia && window.matchMedia('(max-width: 920px)').matches) {
      tocEl.style.top = '';
      return;
    }
    var top = Math.max(8, Math.round((window.innerHeight - tocEl.offsetHeight) / 2));
    tocEl.style.top = top + 'px';
  }

  function onJourneyResize() {
    positionToc();
    onJourneyScroll();
  }

  function setupJourney() {
    // Render each slide's demo a little before it scrolls in, so neighbours hold
    // content while they sit dimmed in the depth-of-field (the animation itself is
    // replayed by refreshActive once the slide reaches the live zone).
    renderIO = new IntersectionObserver(function(entries) {
      entries.forEach(function(e) {
        if (e.isIntersecting) renderSlide(Number(e.target.getAttribute('data-global')) || 0);
      });
    }, { rootMargin: '100% 0px 100% 0px', threshold: 0 });
    for (var i = 0; i < slideEls.length; i++) {
      renderIO.observe(slideEls[i]);
    }
    window.addEventListener('scroll', onJourneyScroll, { passive: true });
    window.addEventListener('resize', onJourneyResize);
    // A real user gesture mid-jump is fresh intent and must win: cancel the pending
    // jump so the magnet stops pulling back to the clicked slide. (Our own smooth
    // scroll fires `scroll`, not wheel/touchstart, so this never cancels the jump.)
    window.addEventListener('wheel', cancelJumpIntent, { passive: true });
    window.addEventListener('touchstart', cancelJumpIntent, { passive: true });
    positionToc();                       // centre the TOC for this build/viewport
    window.setTimeout(positionToc, 400); // re-centre once fonts/layout have settled
    refreshActive();                     // set the initial live slide
  }

  function cancelJumpIntent() { jumpIntent = -1; }

  // Smooth-scroll a slide so its DEMO sits at the viewport centre -- the same target
  // the magnet uses, so they agree (no double-motion). Used by TOC clicks + arrows.
  // Records the slide as the authoritative jump intent: the magnet serves it (it
  // cannot hijack the scroll to a slide we pass on the way), and the highlight
  // locks to it at once so the TOC reflects the click before the scroll arrives.
  function jumpToGlobal(g) {
    if (!demoEls || !demoEls.length) return;
    g = Math.max(0, Math.min(demoEls.length - 1, g));
    jumpIntent = g;
    setHighlight(g);
    var r = demoEls[g].getBoundingClientRect();
    var delta = (r.top + r.height / 2) - window.innerHeight / 2;
    window.scrollTo({ top: window.pageYOffset + delta, behavior: 'smooth' });
  }

  function rebuild(persona) {
    if (renderIO) { renderIO.disconnect(); renderIO = null; }
    if (snapTimer) { window.clearTimeout(snapTimer); snapTimer = null; }
    isSnapping = false;
    snapTarget = -1;
    jumpIntent = -1;
    window.removeEventListener('scroll', onJourneyScroll);
    window.removeEventListener('resize', onJourneyResize);
    window.removeEventListener('wheel', cancelJumpIntent);
    window.removeEventListener('touchstart', cancelJumpIntent);
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

  // Up/Down arrows step between slides while the journey owns the screen (the magnet
  // otherwise swallows their small native scroll). At a boundary they step out of the
  // journey instead of fighting the magnet. Ignored when typing or with modifiers.
  document.addEventListener('keydown', function(event) {
    if (event.defaultPrevented || event.metaKey || event.ctrlKey || event.altKey || event.shiftKey) return;
    if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return;
    var el = event.target;
    if (el && (el.isContentEditable || el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.tagName === 'SELECT')) return;
    if (!journeyEngaged() || currentActive < 0) return;   // only drive the journey while it owns the screen
    var down = event.key === 'ArrowDown';
    var next = currentActive + (down ? 1 : -1);
    event.preventDefault();
    if (next < 0 || next >= demoEls.length) {
      window.scrollBy({ top: (down ? 1 : -1) * window.innerHeight * 0.92, behavior: 'smooth' });
    } else {
      jumpToGlobal(next);
    }
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
