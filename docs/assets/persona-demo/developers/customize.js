// Developers · section 4 · "Customize completely"
// Slides: tune lemond at runtime, then bundle a private branded deployment.
(function () {
  var P = window.LemonadePersona;
  if (!P) return;
  var h = P.helpers;

  P.registerSection('developers', 4, {
    title: 'Customize completely',
    slides: [
      {
        label: 'Tune at runtime',
        demo: 'terminal-customize-runtime',
        caption: 'Set models, context, and backends from the command line or live API.',
        captionHref: 'https://lemonade-server.ai/docs/guide/configuration/',
        animationMode: 'once'
      },
      {
        label: 'Bundle for deployment',
        demo: 'terminal-customize-bundle',
        caption: 'Bundle private models and backends, then launch hidden and locked to your app.',
        captionHref: 'https://lemonade-server.ai/docs/embeddable/',
        animationMode: 'once'
      }
    ]
  });

  var RUNTIME_LINES = [
    { text: '# Tune the runtime for your app', kind: 'comment', phase: 0, delay: 160 },
    { text: '$ lemonade config set max_loaded_models=3 ctx_size=8192 llamacpp.backend=rocm', kind: 'command', phase: 0, delay: 520 },
    { text: '✓ max_loaded_models = 3', kind: 'output', phase: 0, delay: 880 },
    { text: '✓ ctx_size = 8192', kind: 'output', phase: 0, delay: 1060 },
    { text: '✓ llamacpp.backend = rocm', kind: 'output', phase: 0, delay: 1240 },
    { text: '', delay: 1520 },
    { text: '# ...or live, with no restart', kind: 'comment', phase: 1, delay: 1800 },
    { text: '$ curl :13305/internal/set -d \'{"ctx_size":16384}\'', kind: 'command', phase: 1, delay: 2120 },
    { text: '✓ applied', kind: 'output', phase: 1, delay: 2420 }
  ];

  var BUNDLE_LINES = [
    { text: '# Bundle a private, branded deployment', kind: 'comment', phase: 0, delay: 160 },
    { text: '$ lemonade config set models_dir="./models"', kind: 'command', phase: 0, delay: 480 },
    { text: '$ lemonade backends install llamacpp:vulkan', kind: 'command', phase: 0, delay: 760 },
    { text: '✓ bundled into your app folder', kind: 'output', phase: 0, delay: 1080 },
    { text: '', delay: 1360 },
    { text: '# launch hidden, locked to your app', kind: 'comment', phase: 1, delay: 1660 },
    { text: '$ LEMONADE_API_KEY=app-secret lemond ./ --port 13305', kind: 'command', phase: 1, delay: 1980 },
    { text: '✓ lemond running (private)', kind: 'output', phase: 1, delay: 2300 }
  ];

  P.registerDemo('terminal-customize-runtime', function(frame) { frame.innerHTML = h.renderTerminal('Bash', RUNTIME_LINES); });
  P.registerDemo('terminal-customize-bundle', function(frame) { frame.innerHTML = h.renderTerminal('Bash', BUNDLE_LINES); });
})();
