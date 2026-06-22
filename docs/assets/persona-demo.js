// ============================================================================
// Persona-demo hero
// ----------------------------------------------------------------------------
// Drives the interactive hero on the homepage: the People/Developers persona
// toggle, the step "stack" with its autoplay progress bar, and the demo frame
// that renders terminals, app mockups, and — for the developer persona — the
// router flowcharts.
//
// The flowchart diagrams themselves live in flowchart.js (window.LemonadeFlowchart);
// this module just hands that renderer its autoplay cadence so the animation
// cycle stays locked to the progress bar. Requires flowchart.js to load first.
// ============================================================================
(function () {
  var stackEl = document.getElementById('personaStack');
  var demoEl = document.getElementById('personaDemoFrame');
  var captionEl = document.getElementById('personaDemoCaption');
  var titleEl = document.getElementById('personaHeroTitle');
  var subtitleEl = document.getElementById('personaHeroSubtitle');
  var autoplayToggle = document.getElementById('personaAutoplay');
  if (!stackEl || !demoEl || !captionEl || !titleEl || !subtitleEl) return;

  var activeStep = 0;
  var activeSlide = 0;
  var autoplayTimer = null;
  var autoplayPaused = false;
  var slideShownAt = Date.now();
  var defaultAutoplayDelay = 5200;
  var animationSubsectionDelay = 2450;
  var animationSubsectionGap = 350;
  var personaSteps = {
    people: {
      title: 'Run AI on your personal hardware.',
      subtitle: 'Lemonade is a refreshingly simple, free and open-source way to run AI locally. It optimizes for your device, stays private, and works from desktop, server, or mobile.',
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

  function activeSlideData(persona) {
    var data = personaSteps[persona || currentPersona()] || personaSteps.people;
    var step = data.steps[activeStep] || data.steps[0];
    return step && step.slides ? step.slides[activeSlide] : null;
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

  function renderDemo(step) {
    var slide = step.slides && step.slides[activeSlide];
    var demoKind = (slide && slide.demo) || step.demo;
    var captionText = slide && Object.prototype.hasOwnProperty.call(slide, 'caption') ? slide.caption : (step.copy || '');
    var captionHref = slide && slide.captionHref;
    var mode = animationMode(slide);
    if (captionText && captionHref) {
      captionEl.innerHTML = '<a class="hp-demo-caption-link" href="' + escapeText(captionHref) + '" target="_blank" rel="noopener">' +
        escapeText(captionText) + ' <span class="hp-demo-caption-arrow" aria-hidden="true">&#8594;</span></a>';
    } else {
      captionEl.textContent = captionText;
    }
    captionEl.hidden = !captionText;
    demoEl.setAttribute('data-animation-mode', mode);
    if (demoKind.indexOf('router-') === 0 || demoKind === 'spawn-app' || demoKind === 'deploy-everywhere') {
      // Flowchart diagrams come from the flowchart.js module. Hand it our
      // autoplay cadence so its SMIL cycle stays aligned with the progress bar.
      demoEl.innerHTML = window.LemonadeFlowchart.render(demoKind, {
        subsectionDelay: animationSubsectionDelay,
        subsectionGap: animationSubsectionGap,
        minCycle: defaultAutoplayDelay
      });
    } else if (demoKind.indexOf('explore-') === 0) {
      demoEl.innerHTML = exploreDemo(demoKind);
    } else if (demoKind.indexOf('models-') === 0) {
      demoEl.innerHTML = modelsDemo(demoKind);
    } else if (demoKind.indexOf('apps-') === 0) {
      demoEl.innerHTML = appStore(demoKind);
    } else if (demoKind === 'private-app') {
      demoEl.innerHTML = privateApp();
    } else if (demoKind.indexOf('backend-') === 0) {
      demoEl.innerHTML = backendBoard(demoKind);
    } else {
      demoEl.innerHTML = commandDemo(demoKind);
    }
    startSvgAnimations(demoEl);
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

  function renderStack(persona, stepIndex, slideIndex) {
    var data = personaSteps[persona] || personaSteps.people;
    var steps = data.steps;
    activeStep = Math.max(0, Math.min(stepIndex || 0, steps.length - 1));
    activeSlide = Math.max(0, Math.min(slideIndex || 0, ((steps[activeStep].slides || []).length || 1) - 1));
    titleEl.textContent = data.title;
    subtitleEl.textContent = data.subtitle;
    stackEl.setAttribute('aria-label', data.label);
    stackEl.innerHTML = steps.map(function(step, index) {
      var minorControls = index === activeStep && step.slides ? '<span class="hp-minor-segments" role="group" aria-label="' + escapeText(step.title) + ' slides">' +
        step.slides.map(function(slide, slideIndex) {
          var isActive = slideIndex === activeSlide;
          var mode = animationMode(slide);
          return '<button class="hp-minor-segment' + (isActive ? ' is-active' : '') + '" type="button" data-step="' + index + '" data-slide="' + slideIndex + '" data-animation-mode="' + escapeText(mode) + '">' +
            '<span>' + escapeText(slide.label) + '</span>' +
            (isActive ? '<i class="hp-autoplay-progress" aria-hidden="true"></i>' : '') +
          '</button>';
        }).join('') +
      '</span>' : '';
      return '<div class="hp-stack-item' + (index === activeStep ? ' is-active' : '') + '" role="tab" tabindex="0" aria-selected="' + (index === activeStep ? 'true' : 'false') + '" data-step="' + index + '">' +
        '<span class="hp-stack-index">' + String(index + 1).padStart(2, '0') + '</span>' +
        '<span class="hp-stack-body">' +
          '<strong>' + escapeText(step.title) + '</strong>' +
          (index === activeStep ? '<span class="hp-stack-copy">' + escapeText(step.copy) + '</span>' : '') +
          minorControls +
        '</span>' +
      '</div>';
    }).join('');
    renderDemo(steps[activeStep]);
    slideShownAt = Date.now();
    scheduleAutoplay();
  }

  stackEl.addEventListener('click', function(event) {
    // Selecting a section/slide no longer halts autoplay -- that's the toggle's job.
    var minor = event.target.closest && event.target.closest('.hp-minor-segment');
    if (minor) {
      renderStack(currentPersona(), Number(minor.getAttribute('data-step')) || 0, Number(minor.getAttribute('data-slide')) || 0);
      return;
    }
    var button = event.target.closest && event.target.closest('.hp-stack-item');
    if (!button) return;
    renderStack(currentPersona(), Number(button.getAttribute('data-step')) || 0);
  });

  stackEl.addEventListener('keydown', function(event) {
    if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return;
    pauseAutoplay();
    event.preventDefault();
    var direction = event.key === 'ArrowDown' ? 1 : -1;
    var data = personaSteps[currentPersona()] || personaSteps.people;
    renderStack(currentPersona(), (activeStep + direction + data.steps.length) % data.steps.length);
  });

  function updateAutoplayToggle() {
    if (!autoplayToggle) return;
    var on = !autoplayPaused;
    autoplayToggle.classList.toggle('is-on', on);
    autoplayToggle.setAttribute('aria-checked', on ? 'true' : 'false');
  }

  if (autoplayToggle) {
    autoplayToggle.addEventListener('click', function() {
      autoplayPaused = !autoplayPaused;
      updateAutoplayToggle();
      scheduleAutoplay();
    });
  }

  function nextDepthFirst(persona) {
    var data = personaSteps[persona] || personaSteps.people;
    var steps = data.steps;
    var step = steps[activeStep] || steps[0];
    var slideCount = (step && step.slides && step.slides.length) || 1;
    if (activeSlide + 1 < slideCount) {
      renderStack(persona, activeStep, activeSlide + 1);
      return;
    }
    renderStack(persona, (activeStep + 1) % steps.length, 0);
  }

  function scheduleAutoplay() {
    clearTimeout(autoplayTimer);
    updateAutoplayToggle();
    var slide = activeSlideData(currentPersona());
    // Every slide advances after a single playback cycle; 'repeat' slides still
    // loop visually while displayed (the carousel just moves on after one pass).
    var playbackDuration = playbackCycleDuration(slide);
    var delay = playbackDuration;
    stackEl.style.setProperty('--hp-playback-duration', playbackDuration + 'ms');
    stackEl.setAttribute('data-animation-mode', animationMode(slide));
    if (autoplayPaused || window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
      stackEl.setAttribute('data-autoplay', 'paused');
      return;
    }
    stackEl.setAttribute('data-autoplay', 'running');
    // For a non-looping (play-once) slide, advance when its animation finishes:
    // delay minus however long it's already been on screen (0 if already done,
    // so flipping autoplay on after a finished one-shot advances immediately).
    // Looping ('repeat') slides keep their full per-cycle delay.
    if (animationMode(slide) !== 'repeat') {
      delay = Math.max(0, delay - (Date.now() - slideShownAt));
    }
    autoplayTimer = setTimeout(function() {
      nextDepthFirst(currentPersona());
    }, delay);
  }

  function pauseAutoplay() {
    autoplayPaused = true;
    scheduleAutoplay();
  }

  window.addEventListener('lemonadePersonaChange', function(event) {
    autoplayPaused = false;
    renderStack((event.detail && event.detail.persona) || currentPersona(), 0);
  });
  document.addEventListener('DOMContentLoaded', function() {
    renderStack(currentPersona(), 0);
  });
  renderStack(currentPersona(), 0);
})();
