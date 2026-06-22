// ============================================================================
// Persona-demo hero
// ----------------------------------------------------------------------------
// Drives the homepage persona content: an overview MAP (people = journey path,
// developers = stack diagram) followed by the steps UNROLLED into one scroll
// section each. Each section's demo loops while it is in view (an Intersection
// Observer starts/stops a per-section timer that re-renders the current slide)
// and freezes off-screen. The map collapses into a sticky progress bar as you
// scroll. Persona is toggled by navbar.js (data-persona + lemonadePersonaChange).
//
// The flowchart diagrams live in flowchart.js (window.LemonadeFlowchart); this
// module hands that renderer the loop cadence. Requires flowchart.js first.
// ============================================================================
(function () {
  var titleEl = document.getElementById('personaHeroTitle');
  var subtitleEl = document.getElementById('personaHeroSubtitle');
  var mapEl = document.getElementById('personaMap');
  var journeyEl = document.getElementById('personaJourney');
  var progressEl = document.getElementById('personaProgress');
  var zoneEl = document.getElementById('personaZoneHeading');
  if (!titleEl || !subtitleEl || !mapEl || !journeyEl) return;

  var defaultAutoplayDelay = 5200;       // base in-view loop interval
  var animationSubsectionDelay = 2450;   // flowchart cadence (passed through)
  var animationSubsectionGap = 350;
  var reduceMotion = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  var STEP_ICONS = ['explore', 'database', 'apps', 'terminal', 'dns'];  // people steps
  var personaSteps = {
    people: {
      title: 'Run AI on your personal hardware.',
      subtitle: 'Lemonade is a refreshingly simple, free and open-source way to run AI locally. It optimizes for your device, stays private, and works from desktop, server, or mobile.',
      zone: 'Get to know Lemonade',
      label: 'User journey',
      steps: [
        {
          eyebrow: 'Explore',
          title: 'Explore AI models',
          copy: 'Run chat, image generation, coding, and speech models locally — all from one app.',
          demo: 'explore-chat',
          slides: [
            {
              label: 'Chat with LLMs',
              demo: 'explore-chat',
              caption: 'Chat with local LLMs about anything — fully private, running on your own hardware.',
              duration: 3200
            },
            {
              label: 'Generate and edit images',
              demo: 'explore-images',
              caption: 'Generate and edit images from a text prompt with local image models.',
              duration: 3200
            },
            {
              label: 'Advanced coding agents',
              demo: 'explore-coding',
              caption: 'Power advanced coding agents with models tuned for software development.',
              duration: 3200
            },
            {
              label: 'Transcribe and generate speech',
              demo: 'explore-speech',
              caption: 'Transcribe and generate speech with local audio models.',
              duration: 3200
            }
          ]
        },
        {
          eyebrow: 'Models',
          title: 'Set up your models',
          copy: 'Lemonade helps you manage your library of models.',
          demo: 'models-registry',
          slides: [
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
          demo: 'apps-coding',
          slides: [
            {
              label: 'Coding agents',
              demo: 'apps-coding',
              caption: 'Generate software on your PC with no API costs.',
              captionHref: 'https://lemonade-server.ai/marketplace.html',
              animationMode: 'once',
              duration: 3600
            },
            {
              label: 'Personal agents',
              demo: 'apps-personal',
              caption: 'Keep your data local and private.',
              captionHref: 'https://lemonade-server.ai/marketplace.html',
              animationMode: 'once',
              duration: 3600
            },
            {
              label: 'Productivity',
              demo: 'apps-productivity',
              caption: 'Automate your work with private, local AI.',
              captionHref: 'https://lemonade-server.ai/marketplace.html',
              animationMode: 'once',
              duration: 3600
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

  function animationSubsections(slide) {
    return slide && slide.animationSections && slide.animationSections.length ? slide.animationSections : null;
  }

  function animationMode(slide) {
    if (slide && slide.animationMode) return slide.animationMode;
    return 'once';
  }

  function playbackCycleDuration(slide) {
    if (slide && slide.duration) return slide.duration;
    var sections = animationSubsections(slide);
    if (!sections) return defaultAutoplayDelay;
    return Math.max(defaultAutoplayDelay, sections.length * animationSubsectionDelay + animationSubsectionGap);
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
    return '<div class="hp-demo-terminal">' +
      '<div class="hp-demo-terminal-bar"><span></span><span></span><span></span>' + (demo.title ? '<strong>' + escapeText(demo.title) + '</strong>' : '') + '</div>' +
      '<pre><code>' + terminalCodeHtml(demo.lines) + '</code></pre>' +
    '</div>';
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
    return appWindow(board.title, '<div class="hp-backend-board">' + html + '</div>', 'hp-backend-window is-dark');
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
    } else if (demoKind.indexOf('explore-') === 0) {
      frameEl.innerHTML = exploreDemo(demoKind);
    } else if (demoKind.indexOf('models-') === 0) {
      frameEl.innerHTML = modelsDemo(demoKind);
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

  // ---- Overview map (people = journey path; developers = stack diagram) ------
  // Also fills the condensed sticky progress bar with the same step nodes.
  function buildMap(persona) {
    var data = personaSteps[persona] || personaSteps.people;
    var steps = data.steps;
    var isDev = persona === 'developers';

    if (isDev) {
      // A layered stack: static App / hardware frame rows around the interactive
      // step layers (App-facing at top → hardware at the bottom).
      mapEl.className = 'hp-persona-map is-stack';
      mapEl.innerHTML =
        '<div class="hp-map-layer is-static"><span class="hp-map-label">Your App</span></div>' +
        steps.map(function(step, i) {
          return '<a class="hp-map-layer hp-map-node" href="#persona-step-' + i + '" data-step="' + i + '">' +
              '<span class="hp-map-index">' + (i + 1) + '</span>' +
              '<span class="hp-map-label">' + escapeText(step.title) + '</span>' +
            '</a>';
        }).join('') +
        '<div class="hp-map-layer is-static"><span class="hp-map-label">CPU · GPU · NPU · Cloud</span></div>';
    } else {
      mapEl.className = 'hp-persona-map is-path';
      mapEl.innerHTML = '<div class="hp-map-path">' + steps.map(function(step, i) {
        return '<a class="hp-map-node" href="#persona-step-' + i + '" data-step="' + i + '" style="--idx:' + i + '">' +
            '<span class="hp-map-index">' + (i + 1) + '</span>' +
            '<span class="hp-map-icon"><span class="material-symbols-outlined">' + (STEP_ICONS[i] || 'circle') + '</span></span>' +
            '<span class="hp-map-label">' + escapeText(step.eyebrow || step.title) + '</span>' +
          '</a>';
      }).join('') + '</div>';
    }

    if (progressEl) {
      progressEl.innerHTML = '<div class="hp-progress-inner">' + steps.map(function(step, i) {
        var badge = isDev
          ? '<span class="hp-progress-num">' + (i + 1) + '</span>'
          : '<span class="hp-progress-icon"><span class="material-symbols-outlined">' + (STEP_ICONS[i] || 'circle') + '</span></span>';
        return '<a class="hp-progress-node" href="#persona-step-' + i + '" data-step="' + i + '">' +
            badge + '<span class="hp-progress-label">' + escapeText(step.eyebrow || step.title) + '</span>' +
          '</a>';
      }).join('') + '</div>';
    }
  }

  // ---- Per-step scrollytelling controller ----------------------------------
  // No timers -- the scroll IS the pace. Each slide is a "step" of text; the demo
  // graphic pins (CSS sticky) and, as a step scrolls to the trigger line, the
  // matching slide renders into the pinned graphic and plays its entrance once.
  // The grid-stacked layers cross-fade between slides.
  function makeController(sectionEl, step) {
    var slides = step.slides && step.slides.length ? step.slides : [{ label: step.title, demo: step.demo }];
    return {
      sectionEl: sectionEl,
      frameEl: sectionEl.querySelector('.hp-demo-frame'),
      stepsEls: sectionEl.querySelectorAll('.hp-scrolly-step'),
      layers: sectionEl.querySelectorAll('.hp-demo-slide'),
      step: step,
      slides: slides,
      current: -1,
      // Cross-fade the pinned graphic to slide j. The caption text is static in
      // the scroll steps, so renderDemo gets a null caption element; re-rendering
      // the layer replays that demo's entrance animation as you arrive at it.
      // skipRender just swaps which layer is visible without re-rendering -- used
      // for slide 0, which is rendered once at build and shown (settled) as the
      // section heading scrolls in, so it never "replays" under the heading.
      show: function (j, skipRender) {
        if (j < 0 || j >= this.layers.length) return;
        this.current = j;
        if (!skipRender) renderDemo(this.layers[j], null, this.step, j);
        for (var k = 0; k < this.layers.length; k++) {
          this.layers[k].classList.toggle('is-active', k === j);
        }
        for (var s = 0; s < this.stepsEls.length; s++) {
          this.stepsEls[s].classList.toggle('is-active', s === j);
        }
      }
    };
  }

  var sectionCtrls = [];
  var scroller = null;
  var progressObserver = null;
  var journeyObserver = null;
  var primeObserver = null;

  // ---- Build the scrollytelling sections -----------------------------------
  function buildSections(persona) {
    var data = personaSteps[persona] || personaSteps.people;
    titleEl.textContent = data.title;
    subtitleEl.textContent = data.subtitle;
    if (zoneEl) zoneEl.textContent = data.zone || '';
    journeyEl.setAttribute('aria-label', data.label);
    journeyEl.innerHTML = data.steps.map(function(step, i) {
      var slides = step.slides && step.slides.length ? step.slides : [{ label: step.title, demo: step.demo }];
      // One grid-stacked, cross-faded demo layer per slide (inside the pinned graphic).
      var layers = slides.map(function(slide, j) {
        return '<div class="hp-demo-slide' + (j === 0 ? ' is-active' : '') + '" data-slide="' + j + '"></div>';
      }).join('');
      // One scroll "step" per slide: number + label + caption/link. The section's
      // heading (eyebrow/title/copy) is MERGED into the first step, so "section
      // heading" and "slide 1" are the same scroll beat -- entering the section
      // and reaching slide 1 happen together, with no separate heading region
      // that would show a finished demo and then replay it at slide 1.
      var steps = slides.map(function(slide, j) {
        var cap = slide.caption || '';
        var capHtml = cap
          ? (slide.captionHref
              ? '<a class="hp-demo-caption-link" href="' + escapeText(slide.captionHref) + '" target="_blank" rel="noopener">' +
                  escapeText(cap) + ' <span class="hp-demo-caption-arrow" aria-hidden="true">&#8594;</span></a>'
              : escapeText(cap))
          : '';
        var intro = j === 0
          ? '<div class="hp-scrolly-intro">' +
              '<span class="hp-section-eyebrow">' + escapeText(step.eyebrow || ('0' + (i + 1))) + '</span>' +
              '<h2 class="hp-section-title">' + escapeText(step.title) + '</h2>' +
              '<p class="hp-section-copy">' + escapeText(step.copy || '') + '</p>' +
            '</div>'
          : '';
        return '<div class="hp-scrolly-step' + (j === 0 ? ' is-lead is-active' : '') + '" data-step="' + i + '" data-slide="' + j + '">' +
            intro +
            '<div class="hp-scrolly-row">' +
              '<span class="hp-scrolly-step-num">' + (j + 1) + '</span>' +
              '<div class="hp-scrolly-step-body">' +
                '<span class="hp-scrolly-step-label">' + escapeText(slide.label) + '</span>' +
                (capHtml ? '<p class="hp-scrolly-step-caption">' + capHtml + '</p>' : '') +
              '</div>' +
            '</div>' +
          '</div>';
      }).join('');
      // No hp-reveal here: the generic reveal observer only runs once at load, so
      // a hp-reveal section rebuilt on persona switch would stay invisible. The
      // scrollytelling steps + demos provide their own scroll-driven reveal.
      return '<section class="hp-persona-section hp-scrolly" id="persona-step-' + i + '" data-step="' + i + '" data-side="' + (i % 2 ? 'right' : 'left') + '">' +
          '<div class="hp-scrolly-graphic">' +
            '<div class="hp-demo-frame">' + layers + '</div>' +
          '</div>' +
          '<div class="hp-scrolly-steps">' +
            steps +
          '</div>' +
        '</section>';
    }).join('');

    sectionCtrls = [];
    var sections = journeyEl.querySelectorAll('.hp-persona-section');
    for (var i = 0; i < sections.length; i++) {
      sectionCtrls.push(makeController(sections[i], data.steps[i]));
    }
    // Slide 0 is NOT rendered here: that would fire its entrance animation at
    // page load (before you ever scroll to the section). It's primed lazily as
    // each section approaches the viewport instead (see primeObserver).
  }

  function setActiveStep(i) {
    var nodes = document.querySelectorAll('.hp-map-node, .hp-progress-node');
    for (var k = 0; k < nodes.length; k++) {
      var on = Number(nodes[k].getAttribute('data-step')) === i;
      nodes[k].classList.toggle('is-active', on);
      if (on) nodes[k].setAttribute('aria-current', 'step'); else nodes[k].removeAttribute('aria-current');
    }
  }

  // ---- Scrollama: scroll position drives which slide the pinned graphic shows -
  function setupScroller() {
    // The condensed progress bar is shown ONLY while you're inside the journey
    // zone: scrolled below the overview map, but not yet past the last section.
    // Two observers track those two edges; the bar reflects their AND, so it
    // slides away again once you exit the zone (scroll past the whole journey).
    if (progressEl && mapEl) {
      var pastMap = false, pastJourney = false;
      var syncBar = function () {
        var show = pastMap && !pastJourney;
        progressEl.hidden = !show;
        progressEl.classList.toggle('is-shown', show);
      };
      progressObserver = new IntersectionObserver(function(entries) {
        entries.forEach(function(entry) {
          pastMap = !entry.isIntersecting && entry.boundingClientRect.top < 0;
        });
        syncBar();
      }, { threshold: 0 });
      progressObserver.observe(mapEl);

      journeyObserver = new IntersectionObserver(function(entries) {
        entries.forEach(function(entry) {
          pastJourney = !entry.isIntersecting && entry.boundingClientRect.top < 0;
        });
        syncBar();
      }, { threshold: 0 });
      journeyObserver.observe(journeyEl);
    }
    setActiveStep(0);

    // Prime each section's slide 0 just as the section approaches the viewport,
    // so its entrance animation plays as you scroll in (not at page load). The
    // negative bottom margin fires it once the section's top reaches ~80% down.
    primeObserver = new IntersectionObserver(function(entries) {
      entries.forEach(function(entry) {
        if (!entry.isIntersecting) return;
        var i = Number(entry.target.getAttribute('data-step')) || 0;
        var ctrl = sectionCtrls[i];
        if (ctrl && ctrl.current < 0) ctrl.show(0);   // render + animate slide 0
        primeObserver.unobserve(entry.target);
      });
    }, { threshold: 0, rootMargin: '0px 0px -20% 0px' });
    var primeSecs = journeyEl.querySelectorAll('.hp-persona-section');
    for (var p = 0; p < primeSecs.length; p++) primeObserver.observe(primeSecs[p]);

    // Graceful degradation: if scrollama is unavailable the primed first frames
    // still render on approach -- the page just doesn't advance past slide 0.
    if (!window.scrollama) return;
    scroller = window.scrollama();
    // offset 0.3: the demo grips a slide when its (top-aligned) heading reaches
    // ~the top third / center of the pinned demo, not a third up from the bottom.
    scroller.setup({ step: '.hp-scrolly-step', offset: 0.3 }).onStepEnter(function(resp) {
      var i = Number(resp.element.getAttribute('data-step')) || 0;
      var j = Number(resp.element.getAttribute('data-slide')) || 0;
      var ctrl = sectionCtrls[i];
      // Slides 1+ render so they animate as you scroll to them. Slide 0 skips the
      // re-render only if it was already primed on approach (current === 0), so it
      // doesn't replay under the heading; otherwise (fast scroll) it renders here.
      if (ctrl) ctrl.show(j, j === 0 && ctrl.current === 0);
      setActiveStep(i);
    });
  }

  function rebuild(persona) {
    if (scroller && scroller.destroy) { scroller.destroy(); scroller = null; }
    if (progressObserver) { progressObserver.disconnect(); progressObserver = null; }
    if (journeyObserver) { journeyObserver.disconnect(); journeyObserver = null; }
    if (primeObserver) { primeObserver.disconnect(); primeObserver = null; }
    buildMap(persona);
    buildSections(persona);
    setupScroller();
    if (progressEl) { progressEl.hidden = true; progressEl.classList.remove('is-shown'); }
  }

  // Keep scrollama's trigger math correct when the viewport resizes.
  window.addEventListener('resize', function () { if (scroller && scroller.resize) scroller.resize(); });

  function jumpTo(i) {
    var sec = document.getElementById('persona-step-' + i);
    if (sec) sec.scrollIntoView({ behavior: 'smooth', block: 'start' });
  }

  [mapEl, progressEl].forEach(function(el) {
    if (!el) return;
    el.addEventListener('click', function(event) {
      var node = event.target.closest && event.target.closest('[data-step]');
      if (!node) return;
      event.preventDefault();
      jumpTo(Number(node.getAttribute('data-step')) || 0);
    });
  });

  // Dev CTA "Test-drive" → switch to the User persona via the navbar toggle (so
  // all of navbar.js's side effects fire); fall back to a direct rebuild.
  var testDrive = document.getElementById('devCtaTestDrive');
  if (testDrive) {
    testDrive.addEventListener('click', function() {
      var btn = document.querySelector('[data-persona-option="people"]');
      if (btn) {
        btn.click();
      } else {
        document.documentElement.setAttribute('data-persona', 'people');
        rebuild('people');
      }
      var promise = document.getElementById('personaHeroTitle');
      if (promise && promise.scrollIntoView) promise.scrollIntoView({ behavior: 'smooth', block: 'start' });
    });
  }

  window.addEventListener('lemonadePersonaChange', function(event) {
    rebuild((event.detail && event.detail.persona) || currentPersona());
  });

  function init() { rebuild(currentPersona()); }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})();
