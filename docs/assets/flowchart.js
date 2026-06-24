// ============================================================================
// Flowchart animation framework
// ----------------------------------------------------------------------------
// Self-contained renderer for the glowing-dot "router" diagrams shown in the
// developer-persona hero demo. A single declarative SMIL timeline drives every
// route: a dot emits from the request pill, travels a wire trail to a model,
// and terminates in a response, lighting each frosted-glass pill as it passes.
//
// Public API:
//   window.LemonadeFlowchart.render(kind, timing) -> SVG-markup string
//     kind   : 'router-omni' | 'router-hybrid'
//     timing : optional cadence from the host so the animation cycle lines up
//              with the persona-demo autoplay progress bar:
//              { subsectionDelay, subsectionGap, minCycle } (all ms)
//
// Design boundary: this module owns TIMING (SMIL keyTimes are computed, so they
// cannot live in CSS). Appearance (colors, blur, stroke widths) lives in
// flowchart.css. Keep it that way.
// ============================================================================
(function () {
  function escapeText(text) {
    return String(text).replace(/[&<>"']/g, function (ch) {
      return ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[ch];
    });
  }

  // Cadence handed in by the persona-demo host so the flowchart's SMIL cycle and
  // per-route begin offsets line up with the autoplay progress bar. Defaults
  // match the host's constants so the module also works standalone.
  var cadence = { subsectionDelay: 2450, subsectionGap: 350, minCycle: 5200 };

  var routerLayout = {
    request: { x: 92, y: 210, w: 118, h: 52 },
    response: { x: 528, y: 210, w: 118, h: 52 },
    target: { x: 310, w: 214, h: 46 }
  };

  // ---- Flowchart animation framework -------------------------------------
  // One declarative timeline drives all four router animations. Phase
  // durations are fixed (in ms); within a route the dot moves at constant
  // speed and each half of the journey (request -> model, model -> response)
  // takes exactly the same time, so every route reads identically even
  // though the geometry differs. All SMIL timing is derived from these
  // numbers, so the dot, the wire trail, and the pill glows stay locked
  // together by construction.
  // Phase budget per route. Each route must finish inside its slot before the
  // next begins (cycle 5250ms, routes offset 2800ms -> ~2450ms each), so the
  // sum below stays under that. The holds are deliberately long: most of the
  // route is spent dwelling so the reader can actually SEE the input and output
  // artifacts (travel is quick, dwell is generous).
  var routerFlow = {
    requestHold: 260,   // dot dwells in the request pill while the input shows
    travelHalf: 400,    // request-center -> model-center (and the mirror leg)
    modelHold: 320,     // dot dwells in the model pill while it "thinks"
    responseHold: 860,  // dot dwells in the response pill while the output shows
    dotFade: 200,       // dot fades out after the response hold
    trailFade: 220,     // wire trail fades once the dot enters the next pill
    glowLead: 150,      // pill bloom starts this early as the dot approaches
    glowFade: 200       // pill bloom ramp-down after the dot leaves
  };

  // Shared filter region. filterUnits="userSpaceOnUse" with an explicit region
  // spanning the viewBox is deliberate: the default (objectBoundingBox) derives
  // the region from each element's bounding box, which collapses to zero area for
  // axis-aligned geometry (a horizontal wire has a zero-height bbox) and makes the
  // filtered glow vanish. A fixed user-space region keeps every glow working.
  var FILTER_REGION = 'filterUnits="userSpaceOnUse" x="-40" y="-40" width="700" height="500"';

  function routerGlowFilter() {
    return '<filter id="hpRouterGlow" ' + FILTER_REGION + '>' +
      '<feGaussianBlur stdDeviation="8" result="blur"></feGaussianBlur>' +
      '<feMerge><feMergeNode in="blur"></feMergeNode><feMergeNode in="SourceGraphic"></feMergeNode></feMerge>' +
    '</filter>';
  }

  function routerBloomFilter() {
    return '<filter id="hpRouterBloom" ' + FILTER_REGION + '><feGaussianBlur stdDeviation="13"></feGaussianBlur></filter>';
  }

  function routerCycleMs() {
    return Math.max(cadence.minCycle, 2 * cadence.subsectionDelay + cadence.subsectionGap);
  }

  // Pure-SVG pill (rect + text). No foreignObject / backdrop-filter, so it
  // renders identically in Chrome, WebKit and Firefox, the wire endpoints land
  // exactly on the rect edges, and the translucent fill lets the glowing dot
  // shine through from behind (the frosted-lampshade effect, by construction).
  function routerNode(label, className, node) {
    var x = (node.x - node.w / 2).toFixed(1), y = (node.y - node.h / 2).toFixed(1);
    var rx = Math.min(node.h / 2, 24).toFixed(1);
    return '<g class="hp-router-node-group ' + escapeText(className) + '">' +
      '<rect class="hp-router-node" x="' + x + '" y="' + y + '" width="' + node.w + '" height="' + node.h + '" rx="' + rx + '"></rect>' +
      '<text class="hp-router-node-label" x="' + node.x + '" y="' + (node.y + 5.5) + '" text-anchor="middle">' + escapeText(label) + '</text>' +
    '</g>';
  }

  function routerRoutePoints(target) {
    var request = routerLayout.request;
    var response = routerLayout.response;
    var targetLeft = target.x - target.w / 2;
    var targetRight = target.x + target.w / 2;
    var requestRight = request.x + request.w / 2;
    var responseLeft = response.x - response.w / 2;
    return {
      request: { x: request.x, y: request.y },
      requestExit: { x: requestRight, y: request.y },
      targetIn: { x: targetLeft, y: target.y },
      targetCenter: { x: target.x, y: target.y },
      targetOut: { x: targetRight, y: target.y },
      responseIn: { x: responseLeft, y: response.y },
      response: { x: response.x, y: response.y }
    };
  }

  function routerSegmentPath(start, end) {
    return 'M ' + start.x + ' ' + start.y + ' L ' + end.x + ' ' + end.y;
  }

  function routerDistance(start, end) {
    var dx = end.x - start.x;
    var dy = end.y - start.y;
    return Math.sqrt(dx * dx + dy * dy);
  }

  // Build the full per-route timeline. Distances set how far the dot
  // travels on each leg; we hand each half (request->model, model->response)
  // an identical time budget so the dot keeps a constant speed within a
  // route and every route lasts exactly the same total time. Returns
  // absolute waypoint times (ms), path fractions for the dot's motion, and
  // the per-segment wire lengths the trail needs.
  function routerRouteTiming(points) {
    var dLaunch = routerDistance(points.request, points.requestExit);
    var dIn = routerDistance(points.requestExit, points.targetIn);
    var dCenterIn = routerDistance(points.targetIn, points.targetCenter);
    var dCenterOut = routerDistance(points.targetCenter, points.targetOut);
    var dOut = routerDistance(points.targetOut, points.responseIn);
    var dLand = routerDistance(points.responseIn, points.response);
    var firstHalf = (dLaunch + dIn + dCenterIn) || 1;
    var secondHalf = (dCenterOut + dOut + dLand) || 1;
    var total = firstHalf + secondHalf;

    var launch = routerFlow.requestHold;
    var model = launch + routerFlow.travelHalf;
    var resume = model + routerFlow.modelHold;
    var arrive = resume + routerFlow.travelHalf;
    var end = arrive + routerFlow.responseHold;
    var times = {
      emit: 0,
      launch: launch,
      requestExit: launch + (dLaunch / firstHalf) * routerFlow.travelHalf,
      targetIn: launch + ((dLaunch + dIn) / firstHalf) * routerFlow.travelHalf,
      model: model,
      resume: resume,
      targetOut: resume + (dCenterOut / secondHalf) * routerFlow.travelHalf,
      responseIn: resume + ((dCenterOut + dOut) / secondHalf) * routerFlow.travelHalf,
      arrive: arrive,
      end: end
    };
    return {
      times: times,
      lengths: { inWire: dIn, outWire: dOut },
      keyPoints: {
        request: 0,
        requestExit: dLaunch / total,
        targetIn: (dLaunch + dIn) / total,
        targetCenter: firstHalf / total,
        targetOut: (firstHalf + dCenterOut) / total,
        responseIn: (firstHalf + dCenterOut + dOut) / total,
        response: 1
      }
    };
  }

  // Express a millisecond time as a fraction of the shared cycle so it can
  // feed a SMIL keyTimes list. Clamped to [0,1] for safety.
  function routerFrac(ms) {
    var cycle = routerCycleMs();
    var value = ms / cycle;
    return value < 0 ? 0 : (value > 1 ? 1 : value);
  }

  function routerWirePath(target) {
    var points = routerRoutePoints(target);
    return routerSegmentPath(points.requestExit, points.targetIn) + ' ' +
      routerSegmentPath(points.targetOut, points.responseIn);
  }

  function routerTravelPath(target) {
    var points = routerRoutePoints(target);
    return 'M ' + points.request.x + ' ' + points.request.y +
      ' L ' + points.requestExit.x + ' ' + points.requestExit.y +
      ' L ' + points.targetIn.x + ' ' + points.targetIn.y +
      ' L ' + points.targetCenter.x + ' ' + points.targetCenter.y +
      ' L ' + points.targetOut.x + ' ' + points.targetOut.y +
      ' L ' + points.responseIn.x + ' ' + points.responseIn.y +
      ' L ' + points.response.x + ' ' + points.response.y;
  }

  function routerWire(target) {
    return '<path class="hp-router-wire" d="' + routerWirePath(target) + '"></path>';
  }

  function routerFlowBegin(sequenceIndex) {
    var offset = sequenceIndex * (cadence.subsectionDelay + cadence.subsectionGap);
    return (offset / 1000).toFixed(2) + 's';
  }

  function routerFlowCycle() {
    return (routerCycleMs() / 1000).toFixed(3) + 's';
  }

  function routerFlowRepeat(mode) {
    return mode === 'repeat' ? 'indefinite' : '1';
  }

  function routerTimes(values) {
    return values.map(function(value) {
      return Number(value).toFixed(3).replace(/0+$/, '').replace(/\.$/, '');
    }).join(';');
  }

  // Convert a list of millisecond marks to cycle fractions for a keyTimes
  // list, forcing them strictly increasing (and ending at 1). SMIL rejects
  // duplicate keyTimes, which would otherwise happen when a hold collapses
  // to zero length (e.g. the request bloom that is already lit at t=0).
  function routerMonotonicTimes(msList) {
    var eps = 0.002;
    var prev = -eps;
    var fractions = msList.map(function(ms, index) {
      var value = index === msList.length - 1 ? 1 : routerFrac(ms);
      if (value <= prev) value = prev + eps;
      if (value > 1) value = 1;
      prev = value;
      return value;
    });
    return routerTimes(fractions);
  }

  // A single wire "trail" layer. The glow is drawn on progressively behind
  // the dot via stroke-dashoffset (the revealed length always equals the
  // dot's distance along the leg, so the leading edge sits under the dot),
  // then the whole lit segment fades once the dot crosses into the pill.
  function routerTrailLayer(pathD, length, layerClass, enterMs, exitMs, begin, cycle, repeat) {
    var dash = length.toFixed(2);
    var fEnter = routerFrac(enterMs);
    var fExit = routerFrac(exitMs);
    var fFade = routerFrac(exitMs + routerFlow.trailFade);
    var offsetTimes = routerTimes([0, fEnter, fExit, 1]);
    var opacityTimes = routerTimes([0, fEnter, Math.min(fEnter + 0.004, fExit), fExit, fFade, 1]);
    return '<path class="hp-router-flow-segment ' + escapeText(layerClass) + '" d="' + escapeText(pathD) + '" ' +
        'stroke-dasharray="' + dash + '" stroke-dashoffset="' + dash + '" opacity="0">' +
      '<animate attributeName="stroke-dashoffset" dur="' + cycle + '" begin="' + begin + '" repeatCount="' + repeat + '" values="' + dash + ';' + dash + ';0;0" keyTimes="' + offsetTimes + '"></animate>' +
      '<animate attributeName="opacity" dur="' + cycle + '" begin="' + begin + '" repeatCount="' + repeat + '" values="0;0;1;1;0;0" keyTimes="' + opacityTimes + '"></animate>' +
    '</path>';
  }

  // Both wire legs (request->model, model->response) as halo + core trails.
  function routerTrails(points, timing, sequenceIndex, mode) {
    var begin = routerFlowBegin(sequenceIndex);
    var cycle = routerFlowCycle();
    var repeat = routerFlowRepeat(mode);
    function leg(pathD, length, enterMs, exitMs) {
      return routerTrailLayer(pathD, length, 'hp-router-flow-halo', enterMs, exitMs, begin, cycle, repeat) +
        routerTrailLayer(pathD, length, 'hp-router-flow-core', enterMs, exitMs, begin, cycle, repeat);
    }
    return leg(
        routerSegmentPath(points.requestExit, points.targetIn),
        timing.lengths.inWire,
        timing.times.requestExit,
        timing.times.targetIn
      ) +
      leg(
        routerSegmentPath(points.targetOut, points.responseIn),
        timing.lengths.outWire,
        timing.times.targetOut,
        timing.times.responseIn
      );
  }

  // A frosted bloom that sits behind a pill and lights it as the dot
  // arrives. peakStart..peakEnd is the window the dot is "inside" the pill;
  // the glow leads in slightly before and trails out after.
  function routerPillGlow(node, peakStart, peakEnd, leadIn, begin, cycle, repeat) {
    var rx = (node.w / 2 + 16).toFixed(1);
    var ry = (node.h / 2 + 14).toFixed(1);
    var rampStart = Math.max(0, peakStart - leadIn);
    var times = routerMonotonicTimes([
      0,
      rampStart,
      peakStart,
      peakEnd,
      peakEnd + routerFlow.glowFade,
      0
    ]);
    return '<ellipse class="hp-router-pill-glow" cx="' + node.x + '" cy="' + node.y + '" rx="' + rx + '" ry="' + ry + '" opacity="0">' +
      '<animate attributeName="opacity" dur="' + cycle + '" begin="' + begin + '" repeatCount="' + repeat + '" values="0;0;1;1;0;0" keyTimes="' + times + '"></animate>' +
    '</ellipse>';
  }

  // Only the model pill gets a bloom. The request/response pills are opaque and
  // hold artifacts; a bloom behind them just leaked a blurred pill-shaped halo
  // out the sides (it was trying to shine through a pill that isn't translucent).
  function routerModelGlow(target, timing, sequenceIndex, mode) {
    var t = timing.times;
    return routerPillGlow(target, t.targetIn, t.targetOut, routerFlow.glowLead,
      routerFlowBegin(sequenceIndex), routerFlowCycle(), routerFlowRepeat(mode));
  }

  // The travelling dot. animateMotion follows the full request->response
  // path; holds at the request, model, and response are encoded as repeated
  // keyPoints. Opacity fades the dot in inside the request pill and out
  // after the response hold.
  function routerTraveler(pathD, timing, sequenceIndex, mode) {
    var begin = routerFlowBegin(sequenceIndex);
    var cycle = routerFlowCycle();
    var repeat = routerFlowRepeat(mode);
    var k = timing.keyPoints;
    var t = timing.times;
    var keyPoints = routerTimes([
      k.request,
      k.request,
      k.requestExit,
      k.targetIn,
      k.targetCenter,
      k.targetCenter,
      k.targetOut,
      k.responseIn,
      k.response,
      k.response,
      k.response
    ]);
    var keyTimes = routerTimes([
      0,
      routerFrac(t.launch),
      routerFrac(t.requestExit),
      routerFrac(t.targetIn),
      routerFrac(t.model),
      routerFrac(t.resume),
      routerFrac(t.targetOut),
      routerFrac(t.responseIn),
      routerFrac(t.arrive),
      routerFrac(t.end),
      1
    ]);
    var fadeIn = routerFrac(Math.min(80, t.launch));
    var opacityTimes = routerTimes([0, fadeIn, routerFrac(t.end), routerFrac(t.end + routerFlow.dotFade), 1]);
    return '<circle class="hp-router-traveler-dot" cx="0" cy="0" r="7.4" opacity="0">' +
      '<animateMotion path="' + escapeText(pathD) + '" dur="' + cycle + '" begin="' + begin + '" repeatCount="' + repeat + '" calcMode="linear" keyPoints="' + keyPoints + '" keyTimes="' + keyTimes + '"></animateMotion>' +
      '<animate attributeName="opacity" dur="' + cycle + '" begin="' + begin + '" repeatCount="' + repeat + '" values="0;1;1;0;0" keyTimes="' + opacityTimes + '"></animate>' +
    '</circle>';
  }

  // ========================================================================
  // Request / response artifacts
  // ------------------------------------------------------------------------
  // Each route carries a distinct INPUT (request) and OUTPUT (response). The
  // pill expands vertically into the empty space to host the artifact, then
  // contracts; the reveal rides the same SMIL timeline as the dot, so an input
  // is "carried" out as the dot leaves and an output "lands" as it arrives.
  // The contrast between routes is the semantic-routing story.
  // ========================================================================

  // Expanded pill envelope. h is sized to the tallest content (the ~84px image)
  // plus breathing room; everything else (short text, waveform, glyph) centers
  // comfortably inside it. rest is the collapsed pill height (label state).
  var ARTIFACT = { w: 150, h: 124, rest: 56 };
  var FLOW_IMAGE_URL = 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/generated_image.png';

  // SVG waveform bars (no foreignObject): each bar pulses via its own SMIL
  // animation, and the whole row is gated by the artifact group's opacity --
  // which reliably hides SVG children in every browser (unlike a foreignObject,
  // which WebKit leaves visible through an animated group opacity).
  function routerWaveBars(cx, cy) {
    var n = 19, bw = 3, gap = 2.7, hMax = 50;
    var pattern = [0.36, 0.7, 0.94, 0.55, 0.8, 0.46, 0.66];
    var x0 = cx - (n * bw + (n - 1) * gap) / 2;
    var bars = '';
    for (var i = 0; i < n; i++) {
      var h = hMax * pattern[i % pattern.length], hMin = h * 0.4;
      var x = (x0 + i * (bw + gap)).toFixed(1);
      var begin = (-(i % 7) * 0.09).toFixed(2) + 's';
      var yTall = (cy - h / 2).toFixed(1), yShort = (cy - hMin / 2).toFixed(1);
      bars += '<rect class="hp-router-wavebar" x="' + x + '" width="' + bw + '" y="' + yTall + '" height="' + h.toFixed(1) + '" rx="1.5">' +
        '<animate attributeName="height" dur="1.1s" begin="' + begin + '" repeatCount="indefinite" values="' + h.toFixed(1) + ';' + hMin.toFixed(1) + ';' + h.toFixed(1) + '" keyTimes="0;0.5;1"></animate>' +
        '<animate attributeName="y" dur="1.1s" begin="' + begin + '" repeatCount="indefinite" values="' + yTall + ';' + yShort + ';' + yTall + '" keyTimes="0;0.5;1"></animate>' +
      '</rect>';
    }
    return bars;
  }

  function routerWrap(text, maxChars) {
    var words = String(text).split(' '), lines = [], cur = '';
    for (var i = 0; i < words.length; i++) {
      var w = words[i];
      if (!cur) cur = w;
      else if ((cur + ' ' + w).length <= maxChars) cur += ' ' + w;
      else { lines.push(cur); cur = w; }
    }
    if (cur) lines.push(cur);
    return lines;
  }

  // A centered, word-wrapped <text> block. Returns the markup plus the geometry
  // (lines / startY / lh) callers need to place a tag above or a print-reveal clip.
  function routerWrappedText(text, cx, cy, className, vNudge) {
    var lines = routerWrap(text, 16), lh = 18;
    var startY = cy - (lines.length - 1) * lh / 2 + vNudge;
    var tspans = lines.map(function (ln, i) {
      return '<tspan x="' + cx + '" y="' + (startY + i * lh).toFixed(1) + '">' + escapeText(ln) + '</tspan>';
    }).join('');
    return { block: '<text class="' + className + '" text-anchor="middle">' + tspans + '</text>', lines: lines, startY: startY, lh: lh };
  }

  // When the artifact is active within its route (ms). The input lingers until
  // its output lands (holdEnd = arrive); the output holds through the long
  // response dwell so both are on screen long enough to actually read.
  function routerArtifactWindow(role, t) {
    if (role === 'request') {
      return { appear: t.emit, grown: t.launch, holdEnd: t.arrive, gone: t.arrive + 180 };
    }
    return { appear: t.responseIn, grown: t.arrive, holdEnd: t.end, gone: t.end + 200 };
  }

  function routerArtifactKeyTimes(win) {
    return routerMonotonicTimes([0, win.appear, win.grown, win.holdEnd, win.gone, 0]);
  }

  // The request/response pill is ONE persistent frosted rect that morphs: it is
  // always present (so the container style never changes between "pill mode" and
  // "artifact mode" -- it just grows). One animation, beginning at cycle start,
  // encodes every route's expand/hold/contract; the label cross-fades out while
  // expanded. Routes are non-overlapping, so we lay their stops end to end.
  function routerIOPill(role, cx, label, routes) {
    var cycle = routerFlowCycle();
    var cy = routerLayout.request.y;
    var w = ARTIFACT.w, full = ARTIFACT.h, rest = ARTIFACT.rest;
    var x = (cx - w / 2).toFixed(1);
    var stops = [[0, rest]];
    routes.forEach(function (route) {
      var begin = route.sequenceIndex * (cadence.subsectionDelay + cadence.subsectionGap);
      var win = routerArtifactWindow(role, route.timing.times);
      stops.push([begin + win.appear, rest]);
      stops.push([begin + win.grown, full]);
      stops.push([begin + win.holdEnd, full]);
      stops.push([begin + win.gone, rest]);
    });
    stops.push([routerCycleMs(), rest]);
    var times = routerMonotonicTimes(stops.map(function (s) { return s[0]; }));
    var heightVals = stops.map(function (s) { return s[1]; }).join(';');
    var yVals = stops.map(function (s) { return (cy - s[1] / 2).toFixed(1); }).join(';');
    var labelVals = stops.map(function (s) { return s[1] === rest ? 1 : 0; }).join(';');
    function anim(attr, vals) {
      return '<animate attributeName="' + attr + '" dur="' + cycle + '" begin="0s" repeatCount="indefinite" values="' + vals + '" keyTimes="' + times + '"></animate>';
    }
    return '<rect class="hp-router-iopill" x="' + x + '" y="' + (cy - rest / 2).toFixed(1) + '" width="' + w + '" height="' + rest + '" rx="26">' +
        anim('height', heightVals) + anim('y', yVals) +
      '</rect>' +
      '<text class="hp-router-iopill-label" x="' + cx + '" y="' + (cy + 5.5) + '" text-anchor="middle">' + escapeText(label) +
        anim('opacity', labelVals) +
      '</text>';
  }

  function routerArtifactContent(inner, keyTimes, begin, cycle, repeat) {
    return '<g class="hp-router-artifact-content" opacity="0">' +
      '<animate attributeName="opacity" dur="' + cycle + '" begin="' + begin + '" repeatCount="' + repeat + '" values="0;0;1;1;0;0" keyTimes="' + keyTimes + '"></animate>' +
      inner +
    '</g>';
  }

  // Top-down "printing" reveal for generated text/plan output (one clip path).
  function routerPrintReveal(inner, box, win, begin, cycle, repeat, uid) {
    var id = 'hpPrint-' + uid;
    var times = routerMonotonicTimes([0, win.grown, win.grown + 300, win.holdEnd, win.gone, 0]);
    return '<clipPath id="' + id + '"><rect x="' + box.x + '" y="' + box.y + '" width="' + box.w + '" height="0">' +
      '<animate attributeName="height" dur="' + cycle + '" begin="' + begin + '" repeatCount="' + repeat + '" values="0;0;' + box.h + ';' + box.h + ';' + box.h + ';0" keyTimes="' + times + '"></animate>' +
    '</rect></clipPath>' +
    '<g clip-path="url(#' + id + ')">' + inner + '</g>';
  }

  function routerArtTag(cx, y, text) {
    return '<text class="hp-router-art-tag" x="' + cx + '" y="' + y.toFixed(1) + '" text-anchor="middle">' + escapeText(text) + '</text>';
  }

  function routerArtBadge(cx, y, text) {
    var w = Math.min(142, text.length * 6.0 + 28);
    return '<g class="hp-router-art-badge">' +
      '<rect x="' + (cx - w / 2).toFixed(1) + '" y="' + y + '" width="' + w.toFixed(1) + '" height="19" rx="9.5"></rect>' +
      '<text x="' + cx + '" y="' + (y + 13.5) + '" text-anchor="middle">' + escapeText(text) + '</text>' +
    '</g>';
  }

  // Build the inner SVG content for one artifact spec, centered on (cx, cy).
  function routerArtifactInner(role, spec, cx, win, begin, cycle, repeat, uid) {
    var cy = routerLayout.request.y;
    var top = cy - ARTIFACT.h / 2, bottom = cy + ARTIFACT.h / 2;

    // The tag/badge are only drawn when the spec asks for them, so responses
    // can stay clean (no caption above, no badge below) while request inputs
    // keep their small "what kind of input" label.
    var tag = spec.tag ? routerArtTag(cx, top + 20, spec.tag) : '';

    if (spec.type === 'waveform') {
      return (spec.tag ? routerArtTag(cx, cy - 40, spec.tag) : '') + routerWaveBars(cx, cy);
    }

    if (spec.type === 'image') {
      var iw = 130, ih = 84;
      var ix = (cx - iw / 2).toFixed(1), iy = (cy - ih / 2).toFixed(1);
      var clip = 'hpClip-' + uid, dev = 'hpDev-' + uid;
      var devTimes = routerArtifactKeyTimes(win);
      return '<clipPath id="' + clip + '"><rect x="' + ix + '" y="' + iy + '" width="' + iw + '" height="' + ih + '" rx="12"></rect></clipPath>' +
        '<filter id="' + dev + '" ' + FILTER_REGION + '><feGaussianBlur stdDeviation="7"><animate attributeName="stdDeviation" dur="' + cycle + '" begin="' + begin + '" repeatCount="' + repeat + '" values="7;7;0;0;7;7" keyTimes="' + devTimes + '"></animate></feGaussianBlur></filter>' +
        '<image href="' + escapeText(spec.href) + '" x="' + ix + '" y="' + iy + '" width="' + iw + '" height="' + ih + '" preserveAspectRatio="xMidYMid slice" clip-path="url(#' + clip + ')" filter="url(#' + dev + ')"></image>';
    }

    if (spec.type === 'prompt') {
      var p = routerWrappedText(spec.text, cx, cy, 'hp-router-art-prompt', 5);
      return (spec.tag ? routerArtTag(cx, p.startY - p.lines.length * p.lh / 2 - 12, spec.tag) : '') + p.block;
    }

    if (spec.type === 'text') {
      var o = routerWrappedText(spec.text, cx, cy, 'hp-router-art-out', 6);
      var revealed = routerPrintReveal(o.block, { x: (cx - 66).toFixed(1), y: (o.startY - o.lh).toFixed(1), w: 132, h: o.lines.length * o.lh + 14 }, win, begin, cycle, repeat, uid);
      return tag + revealed + (spec.badge ? routerArtBadge(cx, bottom - 26, spec.badge) : '');
    }

    // Iconic, near-wordless artifact: a big glyph + a one-word label (e.g.
    // "</>" + "Plan"). Carries meaning without a wall of text.
    if (spec.type === 'glyph') {
      return tag +
        '<text class="hp-router-art-glyph" x="' + cx + '" y="' + (cy - 2) + '" text-anchor="middle">' + escapeText(spec.glyph || '') + '</text>' +
        (spec.label ? '<text class="hp-router-art-glyph-label" x="' + cx + '" y="' + (cy + 28) + '" text-anchor="middle">' + escapeText(spec.label) + '</text>' : '') +
        (spec.badge ? routerArtBadge(cx, bottom - 26, spec.badge) : '');
    }

    return '';
  }

  // Per-route content that rides on top of the persistent IO pill (no panel of
  // its own -- the pill is the container). Gated to the route's artifact window.
  function routerArtifact(role, spec, cx, timing, sequenceIndex, mode, uid) {
    var begin = routerFlowBegin(sequenceIndex);
    var cycle = routerFlowCycle();
    var repeat = routerFlowRepeat(mode);
    var win = routerArtifactWindow(role, timing.times);
    var keyTimes = routerArtifactKeyTimes(win);
    return routerArtifactContent(routerArtifactInner(role, spec, cx, win, begin, cycle, repeat, uid), keyTimes, begin, cycle, repeat);
  }

  function routerDiagram(config) {
    var request = Object.assign({}, routerLayout.request, { label: config.request });
    var response = Object.assign({}, routerLayout.response, { label: 'Response' });
    var targets = config.targets.map(function(target) {
      return Object.assign({}, routerLayout.target, target);
    });
    var wires = targets.map(routerWire).join('');
    var nodes = targets.map(function(target) {
      return routerNode(target.label, 'hp-router-target', target);
    }).join('');
    var mode = config.animationMode || 'repeat';
    var routes = config.routes.map(function(def, sequenceIndex) {
      var target = targets[def.target];
      var points = routerRoutePoints(target);
      return {
        target: target,
        points: points,
        timing: routerRouteTiming(points),
        path: routerTravelPath(target),
        sequenceIndex: sequenceIndex,
        request: def.request,
        response: def.response
      };
    });
    var trails = routes.map(function(route) {
      return routerTrails(route.points, route.timing, route.sequenceIndex, mode);
    }).join('');
    var glows = routes.map(function(route) {
      return routerModelGlow(route.target, route.timing, route.sequenceIndex, mode);
    }).join('');
    var dots = routes.map(function(route) {
      return routerTraveler(route.path, route.timing, route.sequenceIndex, mode);
    }).join('');
    // The request/response pills: one persistent morphing rect each, driven by
    // every route's expand/hold/contract.
    var ioPills = routerIOPill('request', request.x, request.label, routes) +
      routerIOPill('response', response.x, response.label, routes);
    var artifacts = routes.map(function(route) {
      var out = '';
      if (route.request) out += routerArtifact('request', route.request, request.x, route.timing, route.sequenceIndex, mode, config.id + '-rq' + route.sequenceIndex);
      if (route.response) out += routerArtifact('response', route.response, response.x, route.timing, route.sequenceIndex, mode, config.id + '-rs' + route.sequenceIndex);
      return out;
    }).join('');
    return '<div class="hp-router-demo ' + escapeText(config.className) + '" data-animation-mode="' + escapeText(mode) + '">' +
      '<svg class="hp-router-flowchart" viewBox="0 0 620 420" aria-hidden="true" focusable="false" preserveAspectRatio="xMidYMid meet">' +
        '<defs>' + routerGlowFilter() + routerBloomFilter() + '</defs>' +
        wires +
        trails +
        glows +
        dots +
        nodes +
        ioPills +
        artifacts +
      '</svg>' +
    '</div>';
  }

  // ========================================================================
  // Spawn diagram -- "Start lemond subprocess"
  // ------------------------------------------------------------------------
  // Rendered inside the same terminal-window chrome as the Fetch slide (dots +
  // title bar, no separate stage). The window runs `start lemond`, which
  // ignites a lemon-slice (lemond) whose six segments hold the AI capabilities
  // -- chat, vision, image, speech, transcription, embeddings -- lighting up in
  // a cascade. Plays ONCE and freezes (it's a one-shot, not a loop).
  // ========================================================================
  // Capability icons as pure SVG geometry, each centred on the origin in a ~22u
  // box (so a `translate(x,y)` group places them exactly). No icon font, no
  // <text>, no ligature/baseline/text-anchor -- identical in every engine. Lines
  // and outlines inherit stroke from .hp-cap-glyph; solid nodes use .hp-cap-dot.
  function capGlyph(name) {
    switch (name) {
      case 'chat':   // speech bubble with a tail
        return '<rect x="-9" y="-8.5" width="18" height="13" rx="3.5"></rect>' +
               '<path d="M -4.5 4.5 L -4.5 9 L 1 4.5"></path>';
      case 'vision': // eye
        return '<path d="M -10 0 Q 0 -7.5 10 0 Q 0 7.5 -10 0 Z"></path>' +
               '<circle class="hp-cap-dot" cx="0" cy="0" r="3.1"></circle>';
      case 'image':  // framed picture: sun + mountain
        return '<rect x="-9.5" y="-8.5" width="19" height="17" rx="2.5"></rect>' +
               '<circle class="hp-cap-dot" cx="-3.5" cy="-3" r="2.1"></circle>' +
               '<path d="M -9 7.5 L -1.5 -1 L 2.5 3.5 L 5.5 0.5 L 9.5 7.5"></path>';
      case 'audio':  // equaliser bars
        return '<line x1="-8" y1="-2.5" x2="-8" y2="2.5"></line>' +
               '<line x1="-4" y1="-6" x2="-4" y2="6"></line>' +
               '<line x1="0" y1="-9.5" x2="0" y2="9.5"></line>' +
               '<line x1="4" y1="-6" x2="4" y2="6"></line>' +
               '<line x1="8" y1="-2.5" x2="8" y2="2.5"></line>';
      case 'mic':    // microphone
        return '<rect x="-3.5" y="-10" width="7" height="13" rx="3.5"></rect>' +
               '<path d="M -7 -0.5 Q -7 7 0 7 Q 7 7 7 -0.5"></path>' +
               '<line x1="0" y1="7" x2="0" y2="11"></line>';
      case 'hub':    // central node with four spokes/nodes
        return '<line x1="0" y1="0" x2="0" y2="-9"></line>' +
               '<line x1="0" y1="0" x2="0" y2="9"></line>' +
               '<line x1="0" y1="0" x2="-9" y2="0"></line>' +
               '<line x1="0" y1="0" x2="9" y2="0"></line>' +
               '<circle class="hp-cap-dot" cx="0" cy="0" r="2.8"></circle>' +
               '<circle class="hp-cap-dot" cx="0" cy="-9.5" r="2.2"></circle>' +
               '<circle class="hp-cap-dot" cx="0" cy="9.5" r="2.2"></circle>' +
               '<circle class="hp-cap-dot" cx="-9.5" cy="0" r="2.2"></circle>' +
               '<circle class="hp-cap-dot" cx="9.5" cy="0" r="2.2"></circle>';
      default:
        return '';
    }
  }

  function spawnDemo() {
    var C = 3400;
    var cycle = (C / 1000).toFixed(3) + 's';
    function sf(ms) { return Math.max(0, Math.min(1, ms / C)); }
    function kt() { return routerTimes(Array.prototype.slice.call(arguments)); }
    // One-shot animation that freezes its final state. SMIL requires keyTimes to
    // end at 1, so if the reveal finishes earlier we hold the last value to the
    // end of the cycle (and fill="freeze" holds it after the cycle too).
    function anim(attr, vals, times) {
      var v = vals.split(';'), k = times.split(';');
      if (Number(k[k.length - 1]) < 1) { v.push(v[v.length - 1]); k.push('1'); }
      return '<animate attributeName="' + attr + '" dur="' + cycle + '" begin="0s" fill="freeze" values="' + v.join(';') + '" keyTimes="' + k.join(';') + '"></animate>';
    }
    function motion(pathD, times) {
      return '<animateMotion path="' + escapeText(pathD) + '" dur="' + cycle + '" begin="0s" fill="freeze" calcMode="linear" keyPoints="0;0;1;1" keyTimes="' + times + '"></animateMotion>';
    }

    // The call sits up top; the lemon fills the rest of the window, centred with
    // balanced ~64u gaps above and below (no running caption -- the lit lemon is
    // the "running" signal, which conserves the vertical space).
    var t = { fire: 550, land: 1150 };
    var cx = 310, cy = 214, Rr = 98, Rf = 90;
    var caps = [
      { glyph: 'chat', a: 90 },        // chat (top wedge)
      { glyph: 'vision', a: 30 },      // vision
      { glyph: 'image', a: -30 },      // image
      { glyph: 'audio', a: -90 },      // speech (bottom)
      { glyph: 'mic', a: -150 },       // transcription
      { glyph: 'hub', a: 150 }         // embeddings
    ];

    // --- the call the app runs to start lemond ---
    var call =
      '<rect class="hp-spawn-callbg" x="222" y="34" width="176" height="26" rx="7" opacity="0">' +
        anim('opacity', '0;0;0.85;0.85;0', kt(0, sf(t.fire - 160), sf(t.fire), sf(t.land), sf(t.land + 350))) +
      '</rect>' +
      '<text class="hp-spawn-call" x="310" y="52" text-anchor="middle"><tspan class="run">&#9656;</tspan> <tspan class="kw">start</tspan> lemond</text>';

    var spark = '<circle class="hp-spawn-spark" r="6" cx="0" cy="0" opacity="0">' +
        motion('M 310 66 L ' + cx + ' ' + cy, kt(0, sf(t.fire), sf(t.land), 1)) +
        anim('opacity', '0;0;1;1;0', kt(0, sf(t.fire), sf(t.fire) + 0.02, sf(t.land), sf(t.land) + 0.04)) +
      '</circle>';

    var flash = '<circle class="hp-spawn-flash" cx="' + cx + '" cy="' + cy + '" r="16" opacity="0">' +
        anim('r', '16;16;120', kt(0, sf(t.land), sf(t.land + 360))) +
        anim('opacity', '0;0;0.8;0', kt(0, sf(t.land), sf(t.land + 70), sf(t.land + 360))) +
      '</circle>';

    // --- the lemon slice (= lemond): rind draws on, flesh + membranes fill ---
    var circ = (2 * Math.PI * Rr).toFixed(1);
    var rind = '<circle class="hp-lemon-rind" cx="' + cx + '" cy="' + cy + '" r="' + Rr + '" stroke-dasharray="' + circ + '" stroke-dashoffset="' + circ + '" opacity="0">' +
        anim('stroke-dashoffset', circ + ';' + circ + ';0', kt(0, sf(t.land), sf(t.land + 400))) +
        anim('opacity', '0;0;1', kt(0, sf(t.land), sf(t.land + 60))) +
      '</circle>';
    var flesh = '<circle class="hp-lemon-flesh" cx="' + cx + '" cy="' + cy + '" r="' + Rf + '" opacity="0">' +
        anim('opacity', '0;0;1', kt(0, sf(t.land + 180), sf(t.land + 440))) +
      '</circle>';
    var memG = '<g opacity="0">' +
        anim('opacity', '0;0;1', kt(0, sf(t.land + 280), sf(t.land + 520)));
    for (var m = 0; m < 6; m++) {
      var ma = m * 60 * Math.PI / 180;
      memG += '<line class="hp-lemon-membrane" x1="' + cx + '" y1="' + cy + '" x2="' + (cx + Rf * Math.cos(ma)).toFixed(1) + '" y2="' + (cy - Rf * Math.sin(ma)).toFixed(1) + '"></line>';
    }
    memG += '<circle class="hp-lemon-hub" cx="' + cx + '" cy="' + cy + '" r="7"></circle></g>';

    // --- capability icons, one per wedge, igniting in a cascade ---
    // Drawn as pure SVG geometry (capGlyph) positioned by a group transform, NOT
    // icon-font <text>. This is correct-by-construction across Chrome/WebKit/
    // Firefox: WebKit mis-centres icon-font ligatures in SVG <text> (it measures
    // text-anchor against the unligated NAME width -- "visibility" vs "mic" -- so
    // each icon shifts by a different amount) and mis-handles dominant-baseline.
    // Geometry has no font, ligature, baseline, or text-anchor dependency.
    var icons = '';
    caps.forEach(function (cap, i) {
      var rad = cap.a * Math.PI / 180;
      var ix = cx + 56 * Math.cos(rad), iy = cy - 56 * Math.sin(rad);
      var ti = t.land + 520 + i * 150;
      icons += '<g class="hp-cap-glyph" transform="translate(' + ix.toFixed(1) + ',' + iy.toFixed(1) + ')" opacity="0">' +
          anim('opacity', '0;0;1', kt(0, sf(ti), sf(ti + 170))) +
          capGlyph(cap.glyph) +
        '</g>';
    });

    // Dark variant of the shared .hp-app-window chrome (the same window the User
    // persona's chatbot uses, in light mode) -- title left, window dots right.
    return '<div class="hp-app-window is-dark hp-spawn-term">' +
      '<div class="hp-app-window-bar">' +
        '<span class="hp-app-window-title">Your App</span>' +
        '<span class="hp-app-window-dots"><i></i><i></i><i></i></span>' +
      '</div>' +
      '<svg class="hp-spawn-svg" viewBox="0 0 620 376" preserveAspectRatio="xMidYMid meet" aria-hidden="true" focusable="false">' +
        '<defs>' + routerGlowFilter() + '</defs>' +
        call + spark + flash + rind + flesh + memG + icons +
      '</svg>' +
    '</div>';
  }

  // Developer "Deploy everywhere": a mini app window at top wired to 4 PCs
  // (AMD laptop, NVIDIA desktop, Apple laptop, Vulkan desktop); a glowing dot
  // travels to each in a repeating cascade and the PC lights up on arrival.
  // Pure SVG + SMIL: device icons are geometry, labels are <text> with an
  // explicit y (NOT dominant-baseline), text-anchor centres horizontally only.
  function deployDemo() {
    var C = 4800;
    var cycle = (C / 1000).toFixed(3) + 's';
    // Repeating <animate>; pads the final keyTime to 1 so SMIL stays valid.
    function ranim(attr, vals, times) {
      var v = vals.split(';'), k = times.split(';');
      if (Number(k[k.length - 1]) < 1) { v.push(v[v.length - 1]); k.push('1'); }
      return '<animate attributeName="' + attr + '" dur="' + cycle + '" begin="0s" repeatCount="indefinite" calcMode="linear" values="' + v.join(';') + '" keyTimes="' + k.join(';') + '"></animate>';
    }
    function f(n) { return (+n).toFixed(4); }
    var appX = 310, appY = 94, pcY = 250;
    var pcs = [
      { label: 'AMD', type: 'laptop', x: 110 },
      { label: 'NVIDIA', type: 'desktop', x: 250 },
      { label: 'Apple', type: 'laptop', x: 390 },
      { label: 'Vulkan', type: 'desktop', x: 530 }
    ];

    function pcIcon(type, cx, cy) {
      if (type === 'laptop') {
        return '<rect x="' + (cx - 28) + '" y="' + (cy - 26) + '" width="56" height="34" rx="3"></rect>' +
               '<path d="M ' + (cx - 34) + ' ' + (cy + 16) + ' L ' + (cx + 34) + ' ' + (cy + 16) + ' L ' + (cx + 28) + ' ' + (cy + 9) + ' L ' + (cx - 28) + ' ' + (cy + 9) + ' Z"></path>';
      }
      return '<rect x="' + (cx - 30) + '" y="' + (cy - 26) + '" width="60" height="40" rx="4"></rect>' +
             '<line x1="' + cx + '" y1="' + (cy + 14) + '" x2="' + cx + '" y2="' + (cy + 22) + '"></line>' +
             '<line x1="' + (cx - 13) + '" y1="' + (cy + 22) + '" x2="' + (cx + 13) + '" y2="' + (cy + 22) + '"></line>';
    }

    // mini app window glyph at top (the wire origin)
    var app =
      '<g class="hp-deploy-app">' +
        '<rect x="250" y="34" width="120" height="58" rx="9"></rect>' +
        '<line x1="250" y1="51" x2="370" y2="51"></line>' +
        '<circle cx="262" cy="43" r="2.6"></circle><circle cx="271" cy="43" r="2.6"></circle><circle cx="280" cy="43" r="2.6"></circle>' +
        '<text class="hp-deploy-applabel" x="310" y="78" text-anchor="middle">Your App</text>' +
      '</g>';

    var wires = '', nodes = '', dots = '';
    pcs.forEach(function (pc, i) {
      var px = pc.x;
      var wire = 'M ' + appX + ' ' + (appY + 4) + ' C ' + appX + ' ' + (appY + 86) + ', ' + px + ' ' + (pcY - 86) + ', ' + px + ' ' + (pcY - 34);
      var s = i / 4;
      wires += '<path class="hp-deploy-wire" d="' + wire + '"></path>';
      nodes += '<g class="hp-deploy-pc">' +
          '<g class="hp-deploy-icon">' + pcIcon(pc.type, px, pcY) + '</g>' +
          '<g class="hp-deploy-glow">' + pcIcon(pc.type, px, pcY) +
            ranim('opacity', '0;0;1;0', '0;' + f(s + 0.11) + ';' + f(s + 0.15) + ';' + f(s + 0.24)) +
          '</g>' +
          '<text class="hp-deploy-label" x="' + px + '" y="' + (pcY + 42) + '" text-anchor="middle">' + escapeText(pc.label) + '</text>' +
        '</g>';
      dots += '<circle class="hp-deploy-dot" r="6" cx="0" cy="0" opacity="0">' +
          '<animateMotion dur="' + cycle + '" begin="0s" repeatCount="indefinite" calcMode="linear" path="' + escapeText(wire) + '" keyPoints="0;0;1;1" keyTimes="0;' + f(s) + ';' + f(s + 0.13) + ';1"></animateMotion>' +
          ranim('opacity', '0;0;1;1;0', '0;' + f(s) + ';' + f(s + 0.01) + ';' + f(s + 0.13) + ';' + f(s + 0.15)) +
        '</circle>';
    });

    return '<div class="hp-app-window is-dark hp-deploy-term">' +
      '<div class="hp-app-window-bar">' +
        '<span class="hp-app-window-title">Your App</span>' +
        '<span class="hp-app-window-dots"><i></i><i></i><i></i></span>' +
      '</div>' +
      '<svg class="hp-deploy-svg" viewBox="0 0 620 376" preserveAspectRatio="xMidYMid meet" aria-hidden="true" focusable="false">' +
        '<defs>' + routerGlowFilter() + '</defs>' +
        wires + app + nodes + dots +
      '</svg>' +
    '</div>';
  }

  // User "Serve the whole household": a sleek Lemonade server (bound to the LAN IP
  // from the Secure-it slide) at the top, wired DOWN to the three household apps,
  // each shown on the device it fits -- Open WebUI on a laptop, Dream Server on a
  // NAS/server tower, Lemonade Mobile on a phone. A warm pulse cascades down each
  // wire and the device's screen lights up on arrival. Same hub-and-spoke layout
  // as "Deploy everywhere", but on a LIGHT stage. Pure SVG + SMIL: device glyphs
  // are stroked geometry, labels are <text> with an explicit y (NOT
  // dominant-baseline); soft shadows/glows come from SVG filters (no foreignObject,
  // no backdrop-filter).
  function householdDemo() {
    var C = 5200;
    var cycle = (C / 1000).toFixed(3) + 's';
    function ranim(attr, vals, times) {
      var v = vals.split(';'), k = times.split(';');
      if (Number(k[k.length - 1]) < 1) { v.push(v[v.length - 1]); k.push('1'); }
      return '<animate attributeName="' + attr + '" dur="' + cycle + '" begin="0s" repeatCount="indefinite" calcMode="linear" values="' + v.join(';') + '" keyTimes="' + k.join(';') + '"></animate>';
    }
    function f(n) { return (+n).toFixed(4); }

    var fanX = 310, fanY = 116;   // wires fan from a single point under the server
    var cy = 284;                 // device row centre
    var devices = [
      { app: 'Open WebUI',      id: 'open-webui',      type: 'laptop', x: 150 },
      { app: 'Dream Server',    id: 'dream-server',    type: 'server', x: 310 },
      { app: 'Lemonade Mobile', id: 'lemonade-mobile', type: 'phone',  x: 470 }
    ];
    var APP_LOGO = 'https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/';

    // The lit "screen" on each device -- where the app is shown. Returns the
    // display rect; the warm glow + frame light it up when the pulse lands.
    function screenRect(type, cx) {
      if (type === 'laptop') return { x: cx - 43, y: cy - 30, w: 86, h: 54, rx: 5 };
      if (type === 'phone')  return { x: cx - 20, y: cy - 36, w: 40, h: 74, rx: 7 };
      return { x: cx - 25, y: cy - 18, w: 50, h: 54, rx: 6 };   // server front panel
    }
    // Square logo box centred in the screen -- the app shown "on" the device.
    function logoBox(type, cx) {
      if (type === 'laptop') return { x: cx - 20, y: cy - 23, s: 40 };
      if (type === 'phone')  return { x: cx - 16, y: cy - 15, s: 32 };
      return { x: cx - 20, y: cy - 11, s: 40 };
    }
    // Stroked device frame (no fill); LEDs/vents carry their own class.
    function deviceOutline(type, cx) {
      if (type === 'laptop') {
        return '<rect x="' + (cx - 43) + '" y="' + (cy - 30) + '" width="86" height="54" rx="5"></rect>' +
               '<path d="M ' + (cx - 56) + ' ' + (cy + 34) + ' L ' + (cx + 56) + ' ' + (cy + 34) + ' L ' + (cx + 45) + ' ' + (cy + 24) + ' L ' + (cx - 45) + ' ' + (cy + 24) + ' Z"></path>';
      }
      if (type === 'phone') {
        return '<rect x="' + (cx - 25) + '" y="' + (cy - 46) + '" width="50" height="92" rx="11"></rect>' +
               '<line x1="' + (cx - 5) + '" y1="' + (cy - 41) + '" x2="' + (cx + 5) + '" y2="' + (cy - 41) + '"></line>' +
               '<line x1="' + (cx - 7) + '" y1="' + (cy + 42) + '" x2="' + (cx + 7) + '" y2="' + (cy + 42) + '"></line>';
      }
      // NAS / server tower: body with status LEDs + a vent line above the panel.
      return '<rect x="' + (cx - 33) + '" y="' + (cy - 44) + '" width="66" height="88" rx="9"></rect>' +
             '<circle class="hh-led" cx="' + (cx - 21) + '" cy="' + (cy - 34) + '" r="2.4"></circle>' +
             '<circle class="hh-led" cx="' + (cx - 13) + '" cy="' + (cy - 34) + '" r="2.4"></circle>' +
             '<line x1="' + (cx - 3) + '" y1="' + (cy - 34) + '" x2="' + (cx + 21) + '" y2="' + (cy - 34) + '"></line>';
    }
    function deviceShadow(type, cx) {
      if (type === 'laptop') return '<ellipse class="hh-shadow" cx="' + cx + '" cy="' + (cy + 38) + '" rx="62" ry="8"></ellipse>';
      if (type === 'phone')  return '<ellipse class="hh-shadow" cx="' + cx + '" cy="' + (cy + 54) + '" rx="30" ry="6.5"></ellipse>';
      return '<ellipse class="hh-shadow" cx="' + cx + '" cy="' + (cy + 50) + '" rx="44" ry="7"></ellipse>';
    }

    var shadows = '', wires = '', blooms = '', screens = '', outlines = '', labels = '', dots = '';
    devices.forEach(function (d, i) {
      // Wire fans from one point below the server, splaying out to each device --
      // the look that makes "Deploy everywhere" feel polished.
      // Connect into each device's actual top edge (devices differ in height, so
      // a fixed end-Y left the shorter laptop floating below its wire).
      var topY = d.type === 'laptop' ? cy - 30 : (d.type === 'phone' ? cy - 46 : cy - 44);
      var wire = 'M ' + fanX + ' ' + fanY + ' C ' + fanX + ' ' + (fanY + 92) + ', ' + d.x + ' ' + (topY - 58) + ', ' + d.x + ' ' + topY;

      var s = i / 3;                       // even cascade across the cycle
      var arrive = s + 0.13;
      // Arrival window, defined relative to arrival and kept safely inside [0,1]
      // for EVERY device -- the third device's window would otherwise run past 1
      // and SMIL would silently drop it (that was the "phone never lights" bug).
      var lit = '0;0;1;1;0';
      var litTimes = '0;' + f(arrive - 0.03) + ';' + f(arrive) + ';' + f(arrive + 0.13) + ';' + f(arrive + 0.19);

      shadows += deviceShadow(d.type, d.x);
      wires += '<path class="hh-wire" d="' + wire + '"></path>';

      blooms += '<ellipse class="hh-halo" cx="' + d.x + '" cy="' + (cy + 2) + '" rx="56" ry="52" opacity="0">' +
          ranim('opacity', lit, litTimes) + '</ellipse>';

      // Screen = white panel + warm backlight (behind the logo) + the app logo +
      // a glowing amber frame. The warm/frame fade in when the pulse lands.
      var sr = screenRect(d.type, d.x);
      var lb = logoBox(d.type, d.x);
      screens +=
        '<rect class="hh-screen" x="' + sr.x + '" y="' + sr.y + '" width="' + sr.w + '" height="' + sr.h + '" rx="' + sr.rx + '"></rect>' +
        '<rect class="hh-screen-warm" x="' + sr.x + '" y="' + sr.y + '" width="' + sr.w + '" height="' + sr.h + '" rx="' + sr.rx + '" opacity="0">' +
          ranim('opacity', '0;0;0.4;0.4;0', litTimes) + '</rect>' +
        '<image href="' + escapeText(APP_LOGO + d.id + '/logo.png') + '" x="' + lb.x + '" y="' + lb.y + '" width="' + lb.s + '" height="' + lb.s + '" preserveAspectRatio="xMidYMid meet"></image>' +
        '<rect class="hh-screen-frame" x="' + sr.x + '" y="' + sr.y + '" width="' + sr.w + '" height="' + sr.h + '" rx="' + sr.rx + '" opacity="0">' +
          ranim('opacity', lit, litTimes) + '</rect>';

      outlines += '<g class="hh-dev">' + deviceOutline(d.type, d.x) + '</g>';
      labels += '<text class="hh-label" x="' + d.x + '" y="' + (cy + 60) + '" text-anchor="middle">' + escapeText(d.app) + '</text>';

      // A layered pulse (soft halo + bright gradient core) glides down the wire.
      dots += '<g opacity="0">' +
          '<animateMotion dur="' + cycle + '" begin="0s" repeatCount="indefinite" calcMode="linear" path="' + escapeText(wire) + '" keyPoints="0;0;1;1" keyTimes="0;' + f(s) + ';' + f(arrive) + ';1"></animateMotion>' +
          ranim('opacity', '0;0;1;1;0', '0;' + f(s) + ';' + f(s + 0.01) + ';' + f(arrive - 0.01) + ';' + f(arrive + 0.02)) +
          '<circle class="hh-dot-halo" r="8"></circle>' +
          '<circle class="hh-dot-core" r="4.6"></circle>' +
          '<circle class="hh-dot-spark" r="1.5" cx="-1.3" cy="-1.4"></circle>' +
        '</g>';
    });

    // Sleek server emblem up top: a 2-unit rack glyph + name + LAN IP, on a soft
    // white card. This is the wire origin.
    var scX = 310, scY = 78, scW = 220, scH = 66;
    var gX = scX - scW / 2 + 36;
    var serverCard =
      '<rect class="hh-card" x="' + (scX - scW / 2) + '" y="' + (scY - scH / 2) + '" width="' + scW + '" height="' + scH + '" rx="14" filter="url(#hhShadow)"></rect>' +
      '<g class="hh-srv-glyph" transform="translate(' + gX + ',' + scY + ')">' +
        '<rect x="-15" y="-13" width="30" height="11" rx="2.5"></rect>' +
        '<rect x="-15" y="2" width="30" height="11" rx="2.5"></rect>' +
        '<line x1="-3" y1="-7.5" x2="9" y2="-7.5"></line>' +
        '<line x1="-3" y1="7.5" x2="9" y2="7.5"></line>' +
        '<circle class="hh-srv-led" cx="-9.5" cy="-7.5" r="1.8"></circle>' +
        '<circle class="hh-srv-led" cx="-9.5" cy="7.5" r="1.8"></circle>' +
      '</g>' +
      '<text class="hh-card-title" x="' + (scX + 20) + '" y="' + (scY - 3) + '" text-anchor="middle">Lemonade Server</text>' +
      '<text class="hh-card-ip" x="' + (scX + 20) + '" y="' + (scY + 17) + '" text-anchor="middle">192.168.1.42</text>';

    var defs = '<defs>' +
        '<filter id="hhShadow" x="-40%" y="-40%" width="180%" height="180%"><feDropShadow dx="0" dy="5" stdDeviation="6" flood-color="#3a2f12" flood-opacity="0.16"></feDropShadow></filter>' +
        '<filter id="hhGlow" x="-120%" y="-120%" width="340%" height="340%"><feGaussianBlur stdDeviation="2.4"></feGaussianBlur></filter>' +
        '<radialGradient id="hhDotFill" cx="50%" cy="38%" r="60%"><stop offset="0%" stop-color="#fffaf0"></stop><stop offset="40%" stop-color="#ffb422"></stop><stop offset="100%" stop-color="#f2920a"></stop></radialGradient>' +
        '<radialGradient id="hhHalo" cx="50%" cy="50%" r="50%"><stop offset="0%" stop-color="rgba(255,198,84,0.46)"></stop><stop offset="52%" stop-color="rgba(255,198,84,0.16)"></stop><stop offset="100%" stop-color="rgba(255,198,84,0)"></stop></radialGradient>' +
        '<linearGradient id="hhScreenOn" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stop-color="rgba(255,237,188,0.92)"></stop><stop offset="100%" stop-color="rgba(255,208,104,0.82)"></stop></linearGradient>' +
      '</defs>';

    return '<div class="hp-household-demo">' +
      '<svg class="hp-household-svg" viewBox="0 0 620 420" preserveAspectRatio="xMidYMid meet" aria-hidden="true" focusable="false">' +
        defs +
        shadows + wires + blooms + screens + outlines + labels +
        serverCard + dots +
      '</svg>' +
    '</div>';
  }

  function routerDemo(kind) {
    if (kind === 'router-hybrid') {
      // Hybrid = right SIZE & LOCATION: an easy ask stays local (fast, private,
      // free); only the hard ask is escalated to the frontier cloud model.
      return routerDiagram({
        id: 'hpHybridRoute',
        className: 'hp-router-hybrid-demo',
        request: 'Prompt',
        targets: [
          { label: 'Large local LLM', y: 126 },
          { label: 'Small local LLM', y: 210 },
          { label: 'Frontier cloud LLM', y: 294 }
        ],
        routes: [
          {
            target: 1,
            request: { type: 'prompt', text: 'Reply to my landlord' },
            response: { type: 'text', text: '“Hi Sam — on it.”' }
          },
          {
            target: 2,
            request: { type: 'prompt', text: 'Build a GPU dashboard' },
            response: { type: 'glyph', glyph: '</>', label: 'Plan' }
          }
        ]
      });
    }
    // Omni = right MODALITY: the router dispatches each input to the model that
    // matches it -- a text prompt to image gen, an audio clip to transcription.
    return routerDiagram({
      id: 'hpOmniRoute',
      className: 'hp-router-omni-demo',
      request: 'Request',
      targets: [
        { label: 'Vision LLM', y: 84 },
        { label: 'Image Model', y: 168 },
        { label: 'Speech Model', y: 252 },
        { label: 'ASR Model', y: 336 }
      ],
      routes: [
        {
          target: 1,
          request: { type: 'prompt', text: 'a renaissance lemonade pitcher' },
          response: { type: 'image', href: FLOW_IMAGE_URL }
        },
        {
          target: 3,
          request: { type: 'waveform' },
          response: { type: 'text', text: '“…the perfect lemonade?”' }
        }
      ]
    });
  }

  window.LemonadeFlowchart = {
    render: function (kind, timing) {
      if (timing) {
        cadence = {
          subsectionDelay: timing.subsectionDelay || cadence.subsectionDelay,
          subsectionGap: timing.subsectionGap || cadence.subsectionGap,
          minCycle: timing.minCycle || cadence.minCycle
        };
      }
      if (kind === 'spawn-app') return spawnDemo();
      if (kind === 'deploy-everywhere') return deployDemo();
      if (kind === 'household-network') return householdDemo();
      return routerDemo(kind);
    }
  };
})();
