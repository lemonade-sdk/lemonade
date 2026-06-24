// People · section 3 · "Learn the CLI"
// Slides: launch a coding agent, the chat REPL, and managing the model library.
(function () {
  var P = window.LemonadePersona;
  if (!P) return;
  var h = P.helpers;

  P.registerSection('people', 3, {
    title: 'Learn the CLI',
    slides: [
      {
        label: 'Launch a coding agent',
        demo: 'terminal-cli-launch',
        caption: 'Run Claude Code, Codex, opencode, or pi on your own model — private, and free.',
        captionHref: 'https://lemonade-server.ai/docs/guide/cli/#options-for-launch',
        animationMode: 'once'
      },
      {
        label: 'Chat REPL',
        demo: 'terminal-cli-chat',
        caption: 'Chat with any model right in your terminal.',
        captionHref: 'https://lemonade-server.ai/docs/guide/cli-chat/',
        animationMode: 'once'
      },
      {
        label: 'Manage your models',
        demo: 'terminal-cli-library',
        caption: 'Browse, pull, and load any model straight from the shell.',
        captionHref: 'https://lemonade-server.ai/docs/guide/cli/#options-for-pull',
        animationMode: 'once'
      }
    ]
  });

  var LAUNCH_LINES = [
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
  ];

  var CHAT_LINES = [
    { text: '$ lemonade chat Qwen3.5-4B-GGUF', kind: 'command', phase: 0, delay: 160 },
    { text: '─── Qwen3.5-4B-GGUF ───  ? /help for shortcuts', kind: 'output', phase: 0, delay: 640 },
    { text: '', delay: 940 },
    { text: '> write a haiku about lemons', kind: 'command', phase: 1, delay: 1240 },
    { text: 'Bright yellow teardrops', kind: 'output', phase: 1, delay: 1620 },
    { text: 'hang from the summer branches—', kind: 'output', phase: 1, delay: 1820 },
    { text: 'tart sunshine in your hand.', kind: 'output', phase: 1, delay: 2020 },
    { text: '', delay: 2220 },
    { text: '> /exit', kind: 'command', phase: 2, delay: 2520 }
  ];

  var LIBRARY_LINES = [
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
  ];

  P.registerDemo('terminal-cli-launch', function(frame) { frame.innerHTML = h.renderTerminal('Bash', LAUNCH_LINES); });
  P.registerDemo('terminal-cli-chat', function(frame) { frame.innerHTML = h.renderTerminal('Bash', CHAT_LINES); });
  P.registerDemo('terminal-cli-library', function(frame) { frame.innerHTML = h.renderTerminal('Bash', LIBRARY_LINES); });
})();
