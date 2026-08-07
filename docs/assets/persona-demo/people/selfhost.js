// People · section 4 · "Self-hosted AI"
// Slides: secure the server + bind LAN-wide (terminal), then the household network
// flowchart (rendered by flowchart.js via helpers.renderFlowchart).
(function () {
  var P = window.LemonadePersona;
  if (!P) return;
  var h = P.helpers;

  P.registerSection('people', 4, {
    title: 'Self-hosted AI',
    slides: [
      {
        label: 'Secure it and go LAN-wide',
        demo: 'terminal-selfhost',
        caption: 'Set an API key, then bind to 0.0.0.0 to serve every device on your network.',
        captionHref: 'https://lemonade-server.ai/docs/guide/configuration/',
        animationMode: 'once'
      },
      {
        label: 'Serve the whole household',
        demo: 'household-network',
        caption: 'Your secured server streams private AI to every device — Open WebUI, Dream Server, and the mobile app.',
        captionHref: 'https://lemonade-server.ai/docs/integrations/open-webui/',
        animationMode: 'repeat'
      }
    ]
  });

  var SELFHOST_LINES = [
    { text: '# Protect your server with an API key', kind: 'comment', phase: 0, delay: 160 },
    { text: '$ export LEMONADE_API_KEY="sk-lemon-••••••••"', kind: 'command', phase: 0, delay: 470 },
    { text: '✓ authentication enabled', kind: 'output', phase: 0, delay: 760 },
    { text: '', delay: 1040 },
    { text: '# Make Lemonade reachable across your LAN', kind: 'comment', phase: 1, delay: 1320 },
    { text: '$ lemonade config set host=0.0.0.0', kind: 'command', phase: 1, delay: 1640 },
    { text: '✓ now serving at http://192.168.1.42:13305', kind: 'output', phase: 1, delay: 1960 }
  ];

  P.registerDemo('terminal-selfhost', function(frame) { frame.innerHTML = h.renderTerminal('Bash', SELFHOST_LINES); });
  P.registerDemo('household-network', function(frame) { h.renderFlowchart(frame, 'household-network'); });
})();
