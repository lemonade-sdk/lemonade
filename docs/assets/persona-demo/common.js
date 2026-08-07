// ============================================================================
// Persona-demo SHARED DEMO PRIMITIVES
// ----------------------------------------------------------------------------
// The cross-section building blocks every persona/section file reuses: the app
// window chrome, the download-list + its single moving cursor, and the terminal
// chrome. These augment window.LemonadePersona.helpers so section files build
// their demos with P.helpers.appWindow(...) / renderTerminal(...) etc. Section-
// specific CONTENT (terminal transcripts, card lists) lives in the section files,
// not here. Requires framework.js first (it creates the namespace).
// ============================================================================
(function () {
  var P = window.LemonadePersona;
  if (!P) return;
  var escapeText = P.helpers.escapeText;

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
  // by the model manager + backends list. Pass either a single category +
  // items, or several labelled groups:
  //   opts = { title, category, items: [{name, meta, downloading, progress, swapAt}] }
  //   opts = { title, groups: [{ category, items: [...] }, ...] }
  // The --row index runs continuously across every element -- category headings
  // included -- so the entrance stagger animates smoothly top-to-bottom, with each
  // group label fading in just before its rows.
  function appWindowList(opts) {
    var groups = opts.groups || [{ category: opts.category, items: opts.items }];
    var seq = 0;
    var body = groups.map(function(group, gi) {
      var category = group.category
        ? '<div class="hp-applist-category' + (gi > 0 ? ' is-secondary' : '') + '" style="--row:' + (seq++) + '">' + escapeText(group.category) + '</div>'
        : '';
      var rows = (group.items || []).map(function(item) {
        var i = seq++;
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
      return category + rows;
    }).join('');
    return appWindow(opts.title, '<div class="hp-applist">' + body + '</div>', 'hp-applist-window');
  }

  // One shared cursor that glides smoothly between the download buttons of an
  // appWindowList (model manager + backend manager), pressing each in --swap-at
  // order. Reads as a single mouse moving down the list. Measures live button
  // positions, so it works for either demo (all rows, or just a couple). One-shot.
  // Deliberately ignores prefers-reduced-motion (see persona-demo.css policy note)
  // so the showcase demo plays on every machine, matching the dev flowcharts.
  function playDownloadCursor(frameEl) {
    var list = frameEl.querySelector('.hp-applist');
    if (!list) return;
    var acts = list.querySelectorAll('.hp-applist-act.is-downloading');
    if (!acts.length) return;

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

  // Turn an array of terminal lines into the styled <span> stream. Each line is a
  // string or { text, kind, phase, delay } -- comments auto-detected by a leading #.
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

  // The terminal is the unified app window with a code body, so it shares the
  // chrome -- title bar + window dots on the right. Section files supply their
  // own title + lines. Unlike the generic dark windows it carries its own
  // `hp-terminal-window` class so it can follow the PAGE theme: a light ice-card
  // shell under the default (light) theme, and the warm bash-dark surface under
  // Midnight (`zest-dark`). See the .hp-terminal-window rules in persona-demo.css.
  function renderTerminal(title, lines) {
    return appWindow(title || 'Bash',
      '<div class="hp-terminal-body"><pre><code>' + terminalCodeHtml(lines) + '</code></pre></div>',
      'hp-terminal-window');
  }

  P.helpers.CURSOR_SVG = CURSOR_SVG;
  P.helpers.appWindow = appWindow;
  P.helpers.appWindowList = appWindowList;
  P.helpers.playDownloadCursor = playDownloadCursor;
  P.helpers.terminalCodeHtml = terminalCodeHtml;
  P.helpers.renderTerminal = renderTerminal;
})();
