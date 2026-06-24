// ============================================================================
// Persona-demo FRAMEWORK
// ----------------------------------------------------------------------------
// The journey engine, with no slide content of its own. It owns the persona
// state (the data-persona attribute, the dev dark scheme, persistence, and the
// toggle-only lemonadePersonaChange signal), the scroll/snap/TOC machinery, and
// a small REGISTRY that the per-persona / per-section files populate:
//
//   window.LemonadePersona.registerPersona(name, meta)
//   window.LemonadePersona.registerSection(persona, index, step)
//   window.LemonadePersona.registerDemo(kind, fn)        // fn(frameEl, {animate})
//   window.LemonadePersona.helpers                       // shared builders
//
// This file MUST load first (it creates the namespace); common.js then augments
// helpers, and the section files register their content. Every script here is a
// synchronous <script> in index.html, so init() (gated to DOMContentLoaded) runs
// only after all sections have registered. The map collapses into a sticky
// progress bar as you scroll; each section's demo loops while in view.
//
// The flowchart diagrams live in flowchart.js (window.LemonadeFlowchart); this
// module hands that renderer the loop cadence via helpers.renderFlowchart.
// Requires flowchart.js first.
// ============================================================================
(function () {
  // ---- Registry storage + public API (created before anything else so the
  // section files always have a target, even on pages with no journey) --------
  var personas = {};        // name -> meta {title, subtitle, zone, zoneSubtitle, label, stepIcons?}
  var sections = {};        // name -> array indexed by section order; each entry is a step {title, slides}
  var demoRegistry = {};    // demo kind -> render fn(frameEl, {animate})

  // Flowchart animation cadence (passed through to flowchart.js render()).
  var defaultAutoplayDelay = 5200;       // min cycle length
  var animationSubsectionDelay = 2450;   // per-subsection duration
  var animationSubsectionGap = 350;

  function escapeText(text) {
    return String(text).replace(/[&<>"']/g, function(ch) {
      return ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[ch];
    });
  }

  // Hand the flowchart renderer our cadence. Section files that own a flowchart
  // slide call this from their registered demo fn, so timing stays here (it's a
  // framework concern) while the kind stays with the section that owns it.
  function renderFlowchart(frameEl, kind) {
    frameEl.innerHTML = window.LemonadeFlowchart.render(kind, {
      subsectionDelay: animationSubsectionDelay,
      subsectionGap: animationSubsectionGap,
      minCycle: defaultAutoplayDelay
    });
  }

  function registerPersona(name, meta) { personas[name] = meta || {}; }
  function registerSection(persona, index, step) {
    if (!sections[persona]) sections[persona] = [];
    sections[persona][index] = step;
  }
  function registerDemo(kind, fn) { demoRegistry[kind] = fn; }

  // Assemble a persona's full data object (meta + ordered, compacted steps) from
  // the registrations. Mirrors the shape the journey builder expects.
  function getPersonaData(name) {
    var meta = personas[name];
    if (!meta) return null;
    var steps = (sections[name] || []).filter(Boolean);   // compact the sparse, index-keyed array
    return Object.assign({}, meta, { steps: steps });
  }

  var helpers = { escapeText: escapeText, renderFlowchart: renderFlowchart };

  window.LemonadePersona = {
    registerPersona: registerPersona,
    registerSection: registerSection,
    registerDemo: registerDemo,
    helpers: helpers
  };

  // The hero + promise are mission-level and static (authored in index.html); they
  // stay identical across personas, so this module never touches them. Only the
  // zone heading and the journey below it are persona-aware.
  var journeyEl = document.getElementById('personaJourney');
  var zoneEl = document.getElementById('personaZoneHeading');
  var zoneSubtitleEl = document.getElementById('personaZoneSubtitle');
  if (!journeyEl) return;   // namespace still exists; the engine just stays idle

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

  // Render one slide's demo into a given section frame + caption. When animate is
  // false the demo is built in its START state -- JS players are not run and CSS
  // animations are frozen by the .hp-slide:not(.is-active) rule -- so an upcoming
  // (not-yet-reached) slide shows its "before" look, never its finished frame.
  // refreshActive replays with animate=true once the slide reaches the live zone.
  // The actual HTML lives in the demo functions registered by the section files;
  // this just looks up the kind, sets the caption + animation-mode, and runs it.
  function renderDemo(frameEl, captionEl, step, slideIndex, animate) {
    if (animate === undefined) animate = true;
    var slide = step.slides && step.slides[slideIndex];
    var demoKind = slide && slide.demo;
    var captionText = slide && Object.prototype.hasOwnProperty.call(slide, 'caption') ? slide.caption : '';
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
    var fn = demoKind && demoRegistry[demoKind];
    if (fn) fn(frameEl, { animate: animate });
    else frameEl.innerHTML = '';
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
      var slides = step.slides && step.slides.length ? step.slides : [{ label: step.title }];
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
    var data = getPersonaData(persona) || getPersonaData('people');
    if (zoneEl) zoneEl.textContent = data.zone || '';
    if (zoneSubtitleEl) zoneSubtitleEl.textContent = data.zoneSubtitle || '';
    globalSlides = flattenSlides(data);

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
      // Icon badges when the persona supplies stepIcons; numeric badges otherwise.
      // Driven by persona meta, so a new persona never edits the framework.
      var badge = data.stepIcons
        ? '<span class="hp-toc-sec-badge"><span class="material-symbols-outlined">' + (data.stepIcons[si] || 'circle') + '</span></span>'
        : '<span class="hp-toc-sec-badge">' + (si + 1) + '</span>';
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
