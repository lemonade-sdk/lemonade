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

  // SMIL keyTimes must end at 1. If the last keyframe lands earlier, hold its
  // final value to the end of the cycle so the timeline stays valid. Every demo's
  // <animate> builder shares this one padding rule.
  function padToCycleEnd(vals, times) {
    var v = vals.split(';'), k = times.split(';');
    if (Number(k[k.length - 1]) < 1) { v.push(v[v.length - 1]); k.push('1'); }
    return { v: v, k: k };
  }
  // Repeating <animate> (loops forever) -- shared by the developer-stage demos
  // (deploy / household / stack).
  function repeatAnim(cycle, attr, vals, times) {
    var p = padToCycleEnd(vals, times);
    return '<animate attributeName="' + attr + '" dur="' + cycle + '" begin="0s" repeatCount="indefinite" calcMode="linear" values="' + p.v.join(';') + '" keyTimes="' + p.k.join(';') + '"></animate>';
  }
  // One-shot <animate> that freezes its final state -- used by the spawn demo.
  function freezeAnim(cycle, attr, vals, times) {
    var p = padToCycleEnd(vals, times);
    return '<animate attributeName="' + attr + '" dur="' + cycle + '" begin="0s" fill="freeze" values="' + p.v.join(';') + '" keyTimes="' + p.k.join(';') + '"></animate>';
  }
  function fixed4(n) { return (+n).toFixed(4); }

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
  // Default phase budget. A diagram can slow itself down (e.g. the hybrid demo,
  // whose three scenarios each need time to read) by passing a `flow` override to
  // routerDiagram; `routerFlow` is reassigned to the merged values per render, so
  // every timing function below picks them up. `cadence` (route spacing) is bumped
  // alongside it so the longer routes still don't overlap.
  var routerFlowBase = {
    requestHold: 260,   // dot dwells in the request pill while the input shows
    travelHalf: 400,    // request-center -> model-center (and the mirror leg)
    modelHold: 320,     // dot dwells in the model pill while it "thinks"
    responseHold: 860,  // dot dwells in the response pill while the output shows
    dotFade: 200,       // dot fades out after the response hold
    trailFade: 220,     // wire trail fades once the dot enters the next pill
    glowLead: 150,      // pill bloom starts this early as the dot approaches
    glowFade: 200       // pill bloom ramp-down after the dot leaves
  };
  var routerFlow = routerFlowBase;

  // Shared filter region. filterUnits="userSpaceOnUse" with an explicit region
  // spanning the viewBox is deliberate: the default (objectBoundingBox) derives
  // the region from each element's bounding box, which collapses to zero area for
  // axis-aligned geometry (a horizontal wire has a zero-height bbox) and makes the
  // filtered glow vanish. A fixed user-space region keeps every glow working.
  // The region must span the TALLEST viewBox that uses these filters (the
  // backend-stack demo is 620x540) and the WIDEST (the hybrid router is 780
  // wide), or a glow on content outside the region gets clipped.
  var FILTER_REGION = 'filterUnits="userSpaceOnUse" x="-40" y="-40" width="880" height="640"';

  function routerGlowFilter() {
    return '<filter id="hpRouterGlow" ' + FILTER_REGION + '>' +
      '<feGaussianBlur stdDeviation="8" result="blur"></feGaussianBlur>' +
      '<feMerge><feMergeNode in="blur"></feMergeNode><feMergeNode in="SourceGraphic"></feMergeNode></feMerge>' +
    '</filter>';
  }

  function routerBloomFilter() {
    return '<filter id="hpRouterBloom" ' + FILTER_REGION + '><feGaussianBlur stdDeviation="13"></feGaussianBlur></filter>';
  }

  // Number of routes in the diagram currently being built. Routes begin at
  // i*(subsectionDelay+subsectionGap); the cycle must be long enough that the
  // last route finishes before the loop restarts. Set synchronously by
  // routerDiagram before any timing is computed. Defaults to 2 (the historical
  // value, so a 2-route diagram yields the exact same cycle as before).
  var routeCount = 2;

  function routerCycleMs() {
    return Math.max(
      cadence.minCycle,
      routeCount * cadence.subsectionDelay + (routeCount - 1) * cadence.subsectionGap
    );
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

  // A richer "tier" card (hybrid demo): a taller frosted rect carrying a pure-SVG
  // icon glyph, the tier name, a value sub-line, and an optional badge (e.g.
  // DEFAULT). Left/right edges still sit at node.x +/- node.w/2 so the wire and
  // dot land on the card by construction, exactly like the plain pill.
  function routerTierNode(node) {
    var x = (node.x - node.w / 2).toFixed(1), y = (node.y - node.h / 2).toFixed(1);
    var left = node.x - node.w / 2;
    var rx = 18;
    var iconCx = left + 40;
    var textX = left + 74;
    var iconScale = node.iconScale || 1.4;
    var badge = '';
    if (node.badge) {
      var bw = node.badge.length * 7.2 + 22;
      var bx = node.x + node.w / 2 - bw - 16;
      var by = node.y - node.h / 2 + 12;
      badge = '<g class="hp-router-tier-badge">' +
        '<rect x="' + bx.toFixed(1) + '" y="' + by.toFixed(1) + '" width="' + bw.toFixed(1) + '" height="20" rx="10"></rect>' +
        '<text x="' + (bx + bw / 2).toFixed(1) + '" y="' + (by + 14).toFixed(1) + '" text-anchor="middle">' + escapeText(node.badge) + '</text>' +
      '</g>';
    }
    return '<g class="hp-router-node-group hp-router-tier-group">' +
      '<rect class="hp-router-node hp-router-tier" x="' + x + '" y="' + y + '" width="' + node.w + '" height="' + node.h + '" rx="' + rx + '"></rect>' +
      '<g class="hp-cap-glyph hp-router-tier-icon" transform="translate(' + iconCx.toFixed(1) + ',' + node.y + ') scale(' + iconScale + ')">' + capGlyph(node.icon) + '</g>' +
      '<text class="hp-router-tier-name" x="' + textX.toFixed(1) + '" y="' + (node.y - 9) + '" text-anchor="start">' + escapeText(node.label) + '</text>' +
      (node.sub ? '<text class="hp-router-tier-sub" x="' + textX.toFixed(1) + '" y="' + (node.y + 17) + '" text-anchor="start">' + escapeText(node.sub) + '</text>' : '') +
      badge +
    '</g>';
  }

  // The smart-router node: a frosted square with a hub glyph and a label below
  // it. The three target wires fan out from its centre.
  function routerHubNode(hub) {
    var x = (hub.x - hub.w / 2).toFixed(1), y = (hub.y - hub.h / 2).toFixed(1);
    var glyphY = hub.y - 14;
    var iconScale = hub.iconScale || 1.7;
    return '<g class="hp-router-node-group hp-router-hub-group">' +
      '<rect class="hp-router-node hp-router-hub" x="' + x + '" y="' + y + '" width="' + hub.w + '" height="' + hub.h + '" rx="16"></rect>' +
      '<g class="hp-cap-glyph hp-router-hub-icon" transform="translate(' + hub.x + ',' + glyphY.toFixed(1) + ') scale(' + iconScale + ')">' + capGlyph('hub') + '</g>' +
      '<text class="hp-router-hub-label" x="' + hub.x + '" y="' + (hub.y + 28) + '" text-anchor="middle">' + escapeText(hub.label || 'router') + '</text>' +
    '</g>';
  }

  // request/response default to the shared layout; the hybrid demo overrides
  // their x (4-column layout) and passes a `hub` waypoint the dot routes
  // through between the request pill and the chosen target. Omni passes neither,
  // so its two-point request->target path is byte-for-byte unchanged.
  function routerRoutePoints(target, request, response, hub) {
    request = request || routerLayout.request;
    response = response || routerLayout.response;
    var targetLeft = target.x - target.w / 2;
    var targetRight = target.x + target.w / 2;
    var requestRight = request.x + request.w / 2;
    var responseLeft = response.x - response.w / 2;
    return {
      request: { x: request.x, y: request.y },
      requestExit: { x: requestRight, y: request.y },
      hub: hub ? { x: hub.x, y: hub.y } : null,
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
    var hasHub = !!points.hub;
    var dLaunch = routerDistance(points.request, points.requestExit);
    // With a hub, the "in" journey is two legs: requestExit->hub (shared trunk)
    // then hub->targetIn (fan-out branch). Without a hub it's one leg, so dHub=0
    // and dIn collapses to the original requestExit->targetIn distance.
    var dHub = hasHub ? routerDistance(points.requestExit, points.hub) : 0;
    var dIn = hasHub ? routerDistance(points.hub, points.targetIn)
                     : routerDistance(points.requestExit, points.targetIn);
    var dCenterIn = routerDistance(points.targetIn, points.targetCenter);
    var dCenterOut = routerDistance(points.targetCenter, points.targetOut);
    var dOut = routerDistance(points.targetOut, points.responseIn);
    var dLand = routerDistance(points.responseIn, points.response);
    var firstHalf = (dLaunch + dHub + dIn + dCenterIn) || 1;
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
      hub: hasHub ? launch + ((dLaunch + dHub) / firstHalf) * routerFlow.travelHalf : null,
      targetIn: launch + ((dLaunch + dHub + dIn) / firstHalf) * routerFlow.travelHalf,
      model: model,
      resume: resume,
      targetOut: resume + (dCenterOut / secondHalf) * routerFlow.travelHalf,
      responseIn: resume + ((dCenterOut + dOut) / secondHalf) * routerFlow.travelHalf,
      arrive: arrive,
      end: end
    };
    return {
      times: times,
      // inWire spans the whole lit "in" path (trunk + branch) so the trail
      // dash length matches the polyline the dot actually travels.
      lengths: { inWire: dHub + dIn, outWire: dOut },
      keyPoints: {
        request: 0,
        requestExit: dLaunch / total,
        hub: hasHub ? (dLaunch + dHub) / total : null,
        targetIn: (dLaunch + dHub + dIn) / total,
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

  // Static dim wire. With a hub, the per-target "in" wire is just the fan-out
  // branch (hub->targetIn); the shared requestExit->hub trunk is drawn once by
  // routerDiagram so the three branches visibly emanate from the hub.
  function routerWirePath(target, request, response, hub) {
    var points = routerRoutePoints(target, request, response, hub);
    var inPath = hub ? routerSegmentPath(points.hub, points.targetIn)
                     : routerSegmentPath(points.requestExit, points.targetIn);
    return inPath + ' ' + routerSegmentPath(points.targetOut, points.responseIn);
  }

  function routerTravelPath(target, request, response, hub) {
    var points = routerRoutePoints(target, request, response, hub);
    var d = 'M ' + points.request.x + ' ' + points.request.y +
      ' L ' + points.requestExit.x + ' ' + points.requestExit.y;
    if (hub) d += ' L ' + points.hub.x + ' ' + points.hub.y;
    return d +
      ' L ' + points.targetIn.x + ' ' + points.targetIn.y +
      ' L ' + points.targetCenter.x + ' ' + points.targetCenter.y +
      ' L ' + points.targetOut.x + ' ' + points.targetOut.y +
      ' L ' + points.responseIn.x + ' ' + points.responseIn.y +
      ' L ' + points.response.x + ' ' + points.response.y;
  }

  function routerWire(target, request, response, hub) {
    return '<path class="hp-router-wire" d="' + routerWirePath(target, request, response, hub) + '"></path>';
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
    // The lit "in" path follows the trunk through the hub then the branch out to
    // the target, so the trail heats the same polyline the dot rides.
    var inPath = points.hub
      ? 'M ' + points.requestExit.x + ' ' + points.requestExit.y +
        ' L ' + points.hub.x + ' ' + points.hub.y +
        ' L ' + points.targetIn.x + ' ' + points.targetIn.y
      : routerSegmentPath(points.requestExit, points.targetIn);
    return leg(
        inPath,
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

  // The smart-router hub lights briefly as the dot passes through it -- this IS
  // the routing decision. The peak window is a short dwell centred on t.hub.
  function routerHubGlow(hub, timing, sequenceIndex, mode) {
    var t = timing.times;
    if (t.hub == null) return '';
    return routerPillGlow(hub, Math.max(0, t.hub - 70), t.hub + 70, routerFlow.glowLead,
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
    // Base waypoints: hold at request, launch, exit. A hub (when present) is a
    // single pass-through waypoint inserted between requestExit and targetIn.
    var kp = [k.request, k.request, k.requestExit];
    var kt = [0, routerFrac(t.launch), routerFrac(t.requestExit)];
    if (k.hub != null) {
      kp.push(k.hub);
      kt.push(routerFrac(t.hub));
    }
    kp.push(k.targetIn, k.targetCenter, k.targetCenter, k.targetOut, k.responseIn, k.response, k.response, k.response);
    kt.push(
      routerFrac(t.targetIn),
      routerFrac(t.model),
      routerFrac(t.resume),
      routerFrac(t.targetOut),
      routerFrac(t.responseIn),
      routerFrac(t.arrive),
      routerFrac(t.end),
      1
    );
    var keyPoints = routerTimes(kp);
    var keyTimes = routerTimes(kt);
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
  // opts (all optional): maxChars (wrap width), fontSize (viewBox units, driven
  // inline so it can't drift from the line-height), lineHeight. Defaults match
  // the original so callers that pass nothing are unchanged.
  function routerWrappedText(text, cx, cy, className, vNudge, opts) {
    opts = opts || {};
    var maxChars = opts.maxChars || 16;
    var lh = opts.lineHeight || 18;
    var lines = routerWrap(text, maxChars);
    var startY = cy - (lines.length - 1) * lh / 2 + vNudge;
    var styleAttr = opts.fontSize ? ' style="font-size:' + opts.fontSize + 'px"' : '';
    var tspans = lines.map(function (ln, i) {
      return '<tspan x="' + cx + '" y="' + (startY + i * lh).toFixed(1) + '">' + escapeText(ln) + '</tspan>';
    }).join('');
    return { block: '<text class="' + className + '"' + styleAttr + ' text-anchor="middle">' + tspans + '</text>', lines: lines, startY: startY, lh: lh };
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
  function routerIOPill(role, cx, cy, label, routes) {
    var cycle = routerFlowCycle();
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
  function routerArtifactInner(role, spec, cx, cy, win, begin, cycle, repeat, uid, art) {
    art = art || {};
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
      var p = routerWrappedText(spec.text, cx, cy, 'hp-router-art-prompt', 5,
        { maxChars: art.promptWrap, fontSize: art.promptFont, lineHeight: art.promptLh });
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

    // A summarized checklist: a caption + rows of (check + text bar) that tick in
    // one after another -- the "meeting notes -> action items" output, drawn not
    // typed. Pure SVG; rows reveal via staggered opacity within the dwell window.
    if (spec.type === 'summary') {
      var srows = [92, 80, 66];
      var sStartY = cy - 12, sStep = 27;
      var sOut = routerArtTag(cx, cy - 42, spec.label || 'Action items');
      srows.forEach(function (bw2, i) {
        var ry = sStartY + i * sStep;
        var ckx = cx - 54;
        sOut += '<g opacity="0">' +
          routerRevealAnim(begin, cycle, repeat, win.grown + i * 170, 150) +
          '<circle class="hp-router-check-ring" cx="' + ckx + '" cy="' + ry + '" r="9"></circle>' +
          '<path class="hp-router-check" d="M ' + (ckx - 4.2) + ' ' + ry + ' L ' + (ckx - 1) + ' ' + (ry + 3.6) + ' L ' + (ckx + 4.6) + ' ' + (ry - 3.8) + '"></path>' +
          '<rect class="hp-router-bar" x="' + (cx - 38) + '" y="' + (ry - 5) + '" width="' + bw2 + '" height="10" rx="5"></rect>' +
        '</g>';
      });
      return sOut;
    }

    // A scanned contract: rows of text bars, a couple flagged in red. The red
    // flags pop in after the document lands -- "analyze contract -> red flags".
    if (spec.type === 'flags') {
      var flines = [
        { flag: false, w: 88 }, { flag: true, w: 82 }, { flag: false, w: 88 },
        { flag: true, w: 76 }, { flag: false, w: 84 }
      ];
      var fStartY = cy - 26, fStep = 17, flagN = 0;
      var fOut = routerArtTag(cx, cy - 44, spec.label || 'Contract review');
      flines.forEach(function (ln, i) {
        var ly = fStartY + i * fStep;
        fOut += '<rect class="hp-router-bar' + (ln.flag ? ' hp-router-bar-flagged' : '') + '" x="' + (cx - 26) + '" y="' + (ly - 3.5) + '" width="' + ln.w + '" height="7" rx="3.5"></rect>';
        if (ln.flag) {
          var fx = cx - 50;
          fOut += '<g opacity="0">' +
            routerRevealAnim(begin, cycle, repeat, win.grown + flagN * 240, 170) +
            '<line class="hp-router-flag-pole" x1="' + fx + '" y1="' + (ly - 11) + '" x2="' + fx + '" y2="' + (ly + 8) + '"></line>' +
            '<path class="hp-router-flag" d="M ' + fx + ' ' + (ly - 11) + ' L ' + (fx + 11) + ' ' + (ly - 7.5) + ' L ' + fx + ' ' + (ly - 4) + ' Z"></path>' +
          '</g>';
          flagN++;
        }
      });
      return fOut;
    }

    return '';
  }

  // Staggered fade-in for one element inside an artifact: invisible until
  // appearMs (relative to the route's begin), then a quick fade to full. The
  // parent artifact group handles the shared fade-OUT, so this only ramps in.
  function routerRevealAnim(begin, cycle, repeat, appearMs, fade) {
    var times = routerMonotonicTimes([0, appearMs, appearMs + fade, 0]);
    return '<animate attributeName="opacity" dur="' + cycle + '" begin="' + begin + '" repeatCount="' + repeat + '" values="0;0;1;1" keyTimes="' + times + '"></animate>';
  }

  // Per-route content that rides on top of the persistent IO pill (no panel of
  // its own -- the pill is the container). Gated to the route's artifact window.
  function routerArtifact(role, spec, cx, cy, timing, sequenceIndex, mode, uid, art) {
    var begin = routerFlowBegin(sequenceIndex);
    var cycle = routerFlowCycle();
    var repeat = routerFlowRepeat(mode);
    var win = routerArtifactWindow(role, timing.times);
    var keyTimes = routerArtifactKeyTimes(win);
    return routerArtifactContent(routerArtifactInner(role, spec, cx, cy, win, begin, cycle, repeat, uid, art), keyTimes, begin, cycle, repeat);
  }

  function routerDiagram(config) {
    // Per-render timing: a diagram can slow its routes (`flow`) and widen their
    // spacing (`cadence`) -- the hybrid demo does both so its three scenarios are
    // readable. Set before any timing is computed. Omni passes neither and keeps
    // the snappy defaults.
    routerFlow = config.flow ? Object.assign({}, routerFlowBase, config.flow) : routerFlowBase;
    if (config.cadence) cadence = Object.assign({}, cadence, config.cadence);
    // Set before any timing is computed so the shared cycle fits every route.
    routeCount = config.routes.length;

    var viewW = config.viewW || 620, viewH = config.viewH || 420;
    // Vertical centre of the request/hub/response spine. Defaults to the shared
    // layout's y; the hybrid demo overrides it to the middle of its taller
    // viewBox so the whole composition is balanced (no letterbox dead space).
    var centerY = config.centerY || routerLayout.request.y;
    var request = Object.assign({}, routerLayout.request, { label: config.request, y: centerY });
    var response = Object.assign({}, routerLayout.response, { label: 'Response', y: centerY });
    // The hybrid demo widens the stage and slides the request/response columns
    // to make room for a 4th station (the hub); omni passes none of these.
    if (config.requestX != null) request.x = config.requestX;
    if (config.responseX != null) response.x = config.responseX;
    var targets = config.targets.map(function(target) {
      return Object.assign({}, routerLayout.target, target);
    });
    // Smart-router hub (hybrid only): the dot routes through it and the target
    // wires fan out from its centre.
    var hub = config.hub
      ? Object.assign({ y: centerY, w: 120, h: 108, label: 'router' }, config.hub)
      : null;

    var wires = targets.map(function(target) {
      return routerWire(target, request, response, hub);
    }).join('');
    // The shared request->hub trunk, drawn once so the branches read as fanning
    // out of the hub rather than out of the request pill.
    var trunk = hub
      ? '<path class="hp-router-wire" d="' + routerSegmentPath({ x: request.x + request.w / 2, y: request.y }, { x: hub.x, y: hub.y }) + '"></path>'
      : '';
    var nodes = targets.map(function(target) {
      return target.icon ? routerTierNode(target) : routerNode(target.label, 'hp-router-target', target);
    }).join('');
    var hubNode = hub ? routerHubNode(hub) : '';

    var mode = config.animationMode || 'repeat';
    var routes = config.routes.map(function(def, sequenceIndex) {
      var target = targets[def.target];
      var points = routerRoutePoints(target, request, response, hub);
      return {
        target: target,
        points: points,
        timing: routerRouteTiming(points),
        path: routerTravelPath(target, request, response, hub),
        sequenceIndex: sequenceIndex,
        request: def.request,
        response: def.response
      };
    });
    var trails = routes.map(function(route) {
      return routerTrails(route.points, route.timing, route.sequenceIndex, mode);
    }).join('');
    var glows = routes.map(function(route) {
      return routerModelGlow(route.target, route.timing, route.sequenceIndex, mode) +
        (hub ? routerHubGlow(hub, route.timing, route.sequenceIndex, mode) : '');
    }).join('');
    var dots = routes.map(function(route) {
      return routerTraveler(route.path, route.timing, route.sequenceIndex, mode);
    }).join('');
    // The request/response pills: one persistent morphing rect each, driven by
    // every route's expand/hold/contract.
    var ioPills = routerIOPill('request', request.x, request.y, request.label, routes) +
      routerIOPill('response', response.x, response.y, response.label, routes);
    var art = config.art || {};
    var artifacts = routes.map(function(route) {
      var out = '';
      if (route.request) out += routerArtifact('request', route.request, request.x, request.y, route.timing, route.sequenceIndex, mode, config.id + '-rq' + route.sequenceIndex, art);
      if (route.response) out += routerArtifact('response', route.response, response.x, response.y, route.timing, route.sequenceIndex, mode, config.id + '-rs' + route.sequenceIndex, art);
      return out;
    }).join('');
    return '<div class="hp-router-demo ' + escapeText(config.className) + '" data-animation-mode="' + escapeText(mode) + '">' +
      '<svg class="hp-router-flowchart" viewBox="0 0 ' + viewW + ' ' + viewH + '" aria-hidden="true" focusable="false" preserveAspectRatio="xMidYMid meet">' +
        '<defs>' + routerGlowFilter() + routerBloomFilter() + '</defs>' +
        wires +
        trunk +
        trails +
        glows +
        dots +
        nodes +
        hubNode +
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
      case 'device': // laptop: a screen over a keyboard base
        return '<rect x="-9" y="-8.5" width="18" height="12" rx="1.5"></rect>' +
               '<path d="M -9 3.5 L 9 3.5 L 11.5 7.5 L -11.5 7.5 Z"></path>';
      case 'server': // two stacked rack units, each with a status light
        return '<rect x="-10" y="-9" width="20" height="7.5" rx="2"></rect>' +
               '<rect x="-10" y="1.5" width="20" height="7.5" rx="2"></rect>' +
               '<circle class="hp-cap-dot" cx="-6" cy="-5.25" r="1.4"></circle>' +
               '<circle class="hp-cap-dot" cx="-6" cy="5.25" r="1.4"></circle>' +
               '<line x1="2" y1="-5.25" x2="6" y2="-5.25"></line>' +
               '<line x1="2" y1="5.25" x2="6" y2="5.25"></line>';
      case 'cloud':  // a rounded cloud outline
        return '<path d="M -7 6 Q -12 6 -12 1.5 Q -12 -3 -7 -3 Q -6 -8 0 -8 Q 6 -8 7 -3 Q 12 -3 12 1.5 Q 12 6 7 6 Z"></path>';
      default:
        return '';
    }
  }

  function spawnDemo() {
    var C = 3400;
    var cycle = (C / 1000).toFixed(3) + 's';
    function sf(ms) { return Math.max(0, Math.min(1, ms / C)); }
    function kt() { return routerTimes(Array.prototype.slice.call(arguments)); }
    // One-shot animation that freezes its final state (fill="freeze" holds it
    // after the cycle too).
    function anim(attr, vals, times) { return freezeAnim(cycle, attr, vals, times); }
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

  // Developer "Deploy everywhere": on a dark stage (the same surface as the
  // Smart Router / "The stack" demos -- NOT an app-window chrome). ONE connected
  // flowchart, top to bottom: Your App (with the embedded "lemond" component) ->
  // OPERATING SYSTEMS -> the HARDWARE they run on.
  //  - OS layer: a Windows laptop, Linux as a bare 2x2 distro-logo grid (Ubuntu,
  //    Arch, Debian, Fedora), and a macOS all-in-one (iMac) on the right.
  //  - Hardware layer: four weighty vendor pills (chip glyph + name). Windows and
  //    Linux SHARE AMD / NVIDIA / Vulkan (the Intel-&-other-GPU proxy); macOS sits
  //    directly above Apple. Edges are real: Windows/Linux each branch to all
  //    three x86/GPU pills, macOS only to Apple.
  //  - Flow: stage 1 the app fans a dot to each OS (logos brighten); stage 2 each
  //    OS branches dots down to its hardware (pill rims glow on arrival). No warm-
  //    screen / gold-frame flash on any device -- all behave identically.
  // Repeats; resets at the loop wrap. Pure SVG + SMIL (cross-browser; no
  // foreignObject): glyphs are geometry, labels are <text> with an explicit y.
  function deployDemo() {
    var C = 5200;
    var cycle = (C / 1000).toFixed(3) + 's';
    function ranim(attr, vals, times) { return repeatAnim(cycle, attr, vals, times); }
    var f = fixed4;

    var appX = 310, appY = 108;       // app-window bottom-centre (flow origin)
    var PLAT = 'https://raw.githubusercontent.com/lemonade-sdk/assets/main/platforms/';
    var cy = 198, osNameY = 256, branchY = 268;   // OS-icon centre, name row, wire-out
    var cyHW = 346, pillW = 100, pillH = 48, pillTop = cyHW - pillH / 2;

    // Hardware the OSes run on (Vulkan = the Intel-&-other-GPU proxy). AMD/NVIDIA/
    // Vulkan group under the Windows+Linux pair; Apple sits under macOS.
    var hwPills = {
      amd:    { name: 'AMD',    cx: 110 },
      nvidia: { name: 'NVIDIA', cx: 216 },
      vulkan: { name: 'Vulkan', cx: 322 },
      apple:  { name: 'Apple',  cx: 498 }
    };
    // OS nodes -> the hardware each runs on. Evenly spaced with Linux centred.
    // Windows + Linux SHARE AMD/NVIDIA/Vulkan; macOS sits on the right, directly
    // above Apple. One connected graph: App -> OS -> hardware.
    var oses = [
      { os: 'Windows', cx: 122, type: 'laptop', logos: ['windows.png'], hw: ['amd', 'nvidia', 'vulkan'] },
      { os: 'Linux',   cx: 310, type: 'grid',   logos: ['ubuntu.png', 'arch_linux.png', 'debian.png', 'fedora.png'], hw: ['amd', 'nvidia', 'vulkan'] },
      { os: 'macOS',   cx: 498, type: 'aio',    logos: ['apple.png'], white: true, hw: ['apple'] }
    ];

    var a1 = 0.18, a2 = 0.40;         // app->OS arrival, OS->hardware arrival

    function deviceParts(type, x) {
      if (type === 'aio') {           // all-in-one (iMac): display in a bezel + chin, on a foot
        return { screen: { x: x - 35, y: cy - 27, w: 70, h: 44, rx: 6 }, top: cy - 32,
          base: '<rect x="' + (x - 42) + '" y="' + (cy - 32) + '" width="84" height="60" rx="10"></rect>' +
                '<line x1="' + x + '" y1="' + (cy + 28) + '" x2="' + x + '" y2="' + (cy + 38) + '"></line>' +
                '<line x1="' + (x - 17) + '" y1="' + (cy + 38) + '" x2="' + (x + 17) + '" y2="' + (cy + 38) + '"></line>' };
      }
      return { screen: { x: x - 40, y: cy - 24, w: 80, h: 48, rx: 6 }, top: cy - 24,
        base: '<path d="M ' + (x - 48) + ' ' + (cy + 35) + ' L ' + (x + 48) + ' ' + (cy + 35) + ' L ' + (x + 40) + ' ' + (cy + 24) + ' L ' + (x - 40) + ' ' + (cy + 24) + ' Z"></path>' };
    }
    // Cubic wire with vertical control handles (smooth S between two stacked nodes).
    function vwire(x1, y1, x2, y2, co) {
      return 'M ' + x1 + ' ' + y1 + ' C ' + x1 + ' ' + (y1 + co) + ', ' + x2 + ' ' + (y2 - co) + ', ' + x2 + ' ' + y2;
    }
    // ONE continuous dot per App->OS->hardware edge. It travels the whole path in
    // a single animateMotion (no teleport): app -> into the OS device -> down past
    // the label -> out to the hardware pill. keyPoints/keyTimes pin it so it
    // reaches the OS at a1 and the hardware at a2 regardless of segment lengths.
    // (Edges sharing an OS overlap on the app->OS leg, then branch apart.)
    function flowDot(cx, osTop, hwCx) {
      var co1 = Math.max(16, (osTop - appY) * 0.5);
      var path = 'M ' + appX + ' ' + appY +
        ' C ' + appX + ' ' + (appY + co1) + ', ' + cx + ' ' + (osTop - co1) + ', ' + cx + ' ' + osTop +
        ' L ' + cx + ' ' + branchY +
        ' C ' + cx + ' ' + (branchY + 28) + ', ' + hwCx + ' ' + (pillTop - 28) + ', ' + hwCx + ' ' + pillTop;
      var d1 = Math.hypot(cx - appX, osTop - appY), d2 = branchY - osTop, d3 = Math.hypot(hwCx - cx, pillTop - branchY);
      var kOS = (d1 + d2 * 0.5) / (d1 + d2 + d3 || 1);     // path fraction at the OS device
      return '<circle class="hp-deploy-dot" r="5.5" cx="0" cy="0" opacity="0">' +
          '<animateMotion dur="' + cycle + '" begin="0s" repeatCount="indefinite" calcMode="linear" path="' + escapeText(path) + '" keyPoints="0;0;' + f(kOS) + ';1;1" keyTimes="0;0.04;' + f(a1) + ';' + f(a2) + ';1"></animateMotion>' +
          ranim('opacity', '0;0;1;1;0', '0;0.04;0.07;' + f(a2) + ';' + f(a2 + 0.04)) +
        '</circle>';
    }
    function chipGlyph() {
      var s = '<rect x="-8" y="-8" width="16" height="16" rx="2.5"></rect>' +
              '<rect class="hp-deploy-chip-die" x="-3.5" y="-3.5" width="7" height="7" rx="1.5"></rect>';
      for (var k = -4; k <= 4; k += 4) {
        s += '<line x1="' + k + '" y1="-8" x2="' + k + '" y2="-11.5"></line>' +
             '<line x1="' + k + '" y1="8" x2="' + k + '" y2="11.5"></line>' +
             '<line x1="-8" y1="' + k + '" x2="-11.5" y2="' + k + '"></line>' +
             '<line x1="8" y1="' + k + '" x2="11.5" y2="' + k + '"></line>';
      }
      return s;
    }

    // ---- Build the wires + one continuous dot per edge. Each dot rides the whole
    // App -> OS -> hardware path; OS logos brighten as it passes (a1), hardware
    // pills glow as it lands (a2).
    var wires = '', nodes = '', dots = '';
    var osBright = '0.3;0.3;1;1;0.3', osT = '0;' + f(a1 - 0.06) + ';' + f(a1) + ';0.92;1';
    var labelT = '0.5;0.5;1;1;0.5';
    oses.forEach(function (o) {
      var cx = o.cx;
      var p = o.type === 'grid' ? null : deviceParts(o.type, cx);
      var osTop = p ? p.top : cy - 36;     // grid top (g 19 + gs/2 17)

      // app -> this OS wire (the continuous dot below rides app->OS->hardware).
      var w1 = vwire(appX, appY, cx, osTop, Math.max(16, (osTop - appY) * 0.5));
      wires += '<path class="hp-deploy-wire" d="' + w1 + '"></path>';

      // OS glyph (device + logo, or bare distro grid)
      var glyph = '';
      if (o.type === 'grid') {
        var gs = 34, g = 19;
        var pos = [[cx - g, cy - g], [cx + g, cy - g], [cx - g, cy + g], [cx + g, cy + g]];
        o.logos.forEach(function (file, k) {
          glyph += '<image href="' + PLAT + file + '" x="' + (pos[k][0] - gs / 2) + '" y="' + (pos[k][1] - gs / 2) + '" width="' + gs + '" height="' + gs + '" preserveAspectRatio="xMidYMid meet" opacity="0.3">' + ranim('opacity', osBright, osT) + '</image>';
        });
      } else {
        var sc = p.screen, scCy = sc.y + sc.h / 2;
        var wcls = o.white ? ' class="hp-deploy-logo-white"' : '';
        var ls = Math.min(sc.w, sc.h) - 16;
        glyph = '<image' + wcls + ' href="' + PLAT + o.logos[0] + '" x="' + (cx - ls / 2) + '" y="' + (scCy - ls / 2) + '" width="' + ls + '" height="' + ls + '" preserveAspectRatio="xMidYMid meet" opacity="0.3">' + ranim('opacity', osBright, osT) + '</image>' +
          '<g class="hp-deploy-dev"><rect x="' + sc.x + '" y="' + sc.y + '" width="' + sc.w + '" height="' + sc.h + '" rx="' + sc.rx + '"></rect>' + p.base + '</g>';
      }
      nodes += '<g class="hp-deploy-machine">' + glyph +
          '<text class="hp-deploy-osname" x="' + cx + '" y="' + osNameY + '" text-anchor="middle" opacity="0.5">' +
            escapeText(o.os) + ranim('opacity', labelT, osT) + '</text>' +
        '</g>';

      // OS -> each hardware pill it runs on: a wire + a continuous app->OS->pill dot.
      o.hw.forEach(function (key) {
        var w2 = vwire(cx, branchY, hwPills[key].cx, pillTop, 28);
        wires += '<path class="hp-deploy-wire" d="' + w2 + '"></path>';
        dots += flowDot(cx, osTop, hwPills[key].cx);
      });
    });

    // ---- Hardware pills: weighty cards (chip glyph + vendor name). Each pill's
    // rim glows when its OS-dots arrive (a2). Rendered once per pill.
    var glowT = '0;' + f(a2 - 0.05) + ';' + f(a2) + ';0.92;1';
    var hw = '';
    Object.keys(hwPills).forEach(function (key) {
      var pl = hwPills[key], cxP = pl.cx, rx = cxP - pillW / 2;
      var gw = 22 + 9 + pl.name.length * 8.6, gx = cxP - gw / 2;   // centre chip+name group
      hw += '<rect class="hp-deploy-pill" x="' + rx + '" y="' + pillTop + '" width="' + pillW + '" height="' + pillH + '" rx="12"></rect>' +
        '<rect class="hp-deploy-pill-glow" x="' + rx + '" y="' + pillTop + '" width="' + pillW + '" height="' + pillH + '" rx="12" opacity="0">' + ranim('opacity', '0;0;1;1;0', glowT) + '</rect>' +
        '<g class="hp-deploy-chip" transform="translate(' + (gx + 11).toFixed(1) + ',' + cyHW + ')">' + chipGlyph() + '</g>' +
        '<text class="hp-deploy-vendorname" x="' + (gx + 31).toFixed(1) + '" y="' + (cyHW + 5) + '">' + escapeText(pl.name) + '</text>';
    });

    // "Your App" window -- the title bar names the APP ("Your App"); the body
    // holds the embedded "lemond" component (lemon mark + wordmark), so the
    // graphic reads as an app that INCLUDES lemonade, not an app that IS lemonade.
    var app =
      '<g class="hp-deploy-app">' +
        '<rect x="195" y="14" width="230" height="94" rx="13"></rect>' +
        '<line x1="195" y1="42" x2="425" y2="42"></line>' +
        '<circle cx="212" cy="29" r="3"></circle><circle cx="223" cy="29" r="3"></circle><circle cx="234" cy="29" r="3"></circle>' +
        '<text class="hp-deploy-apptitle" x="310" y="33" text-anchor="middle">Your App</text>' +
        lemonMark(236, 76, 12) +
        '<text class="hp-deploy-lemond" x="262" y="72">lemond</text>' +
        '<text class="hp-deploy-appsub" x="262" y="91">embedded local AI</text>' +
      '</g>';

    // The lemon mark reuses lemonMark's #hpRouterGlow (same as the Engines &
    // Hardware lemon). The earlier "big white glow" was NOT the filter -- it was
    // the translucent window letting the stage's bright gold-radial through behind
    // the lemon, on which the wide glow read as a pale bloom. The opaque-dark
    // window fill (see .hp-deploy-app rect) fixes it: on a flat-dark backdrop the
    // same wide glow spreads thin and faint, exactly like the stack slide.
    return '<div class="hp-deploy-demo">' +
      '<svg class="hp-deploy-svg" viewBox="0 0 620 420" preserveAspectRatio="xMidYMid meet" aria-hidden="true" focusable="false">' +
        '<defs>' + routerGlowFilter() + '</defs>' +
        wires + hw + app + nodes + dots +
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
    function ranim(attr, vals, times) { return repeatAnim(cycle, attr, vals, times); }
    var f = fixed4;

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

  // A compact "lemond" mark: a small lemon slice (rind ring + six segment
  // membranes + hub). Pure geometry so it renders identically everywhere; sized
  // by r so both the stack and models demos can drop it in at any scale.
  function lemonMark(cx, cy, r) {
    var s = '<g class="hp-stack-lemon">' +
      '<circle class="hp-stack-lemon-rind" cx="' + cx + '" cy="' + cy + '" r="' + r + '"></circle>';
    for (var m = 0; m < 6; m++) {
      var a = m * 60 * Math.PI / 180;
      s += '<line class="hp-stack-lemon-seg" x1="' + cx + '" y1="' + cy + '" x2="' +
        (cx + r * Math.cos(a)).toFixed(1) + '" y2="' + (cy - r * Math.sin(a)).toFixed(1) + '"></line>';
    }
    return s + '<circle class="hp-stack-lemon-hub" cx="' + cx + '" cy="' + cy + '" r="' + (r * 0.22).toFixed(1) + '"></circle></g>';
  }

  // Developer "The stack": a software-stack diagram proving lemond is the
  // ABSTRACTION layer. Your App sits on top and makes ONE call across a single
  // API line; below it the lemond slab holds an ENGINES sub-layer sitting on a
  // DEVICES sub-layer (engines are literally a pathway to devices). Each request
  // dips through the API line into the slab, an engine lights (load) then the
  // device it runs on lights (engine -> device), and a clean response rises back
  // out -- the app never reaches past the line, which IS the abstraction. The
  // whole engine/device fleet stays visible at rest (breadth); the motion is the
  // boundary crossing + a focused highlight that steps through representative
  // engine->device pairs. Pure SVG + SMIL (cross-browser; no foreignObject).
  function stackDemo() {
    var C = 14770;   // legible cadence: each of the 3 beats gets ~4.9s
    var cycle = (C / 1000).toFixed(3) + 's';
    function ranim(attr, vals, times) { return repeatAnim(cycle, attr, vals, times); }
    var f = fixed4;

    var appCx = 310;

    // Engines: 8 chips, alphabetical, laid out 2 columns x 4 rows (long names
    // need the width). Devices: 6 chips, 3 columns x 2 rows (short names + a
    // hardware sub-label). Both bands share ONE content box (x 53..567, centre
    // 310): the engine columns and device columns line up on the same outer
    // edges, and the engine gutter sits on the central axis (= the device middle
    // column = the request spine), so the grid reads as one tidy block.
    var engines = [
      'FastFlowLM', 'Kokoro', 'llama.cpp', 'Moonshine',
      'Ryzen AI SW', 'stable-diffusion.cpp', 'vLLM', 'whisper.cpp'
    ];
    var engCols = [177, 443], engRows = [158, 202, 246, 290];
    var engPos = {};
    engines.forEach(function (name, i) {
      engPos[name] = { cx: engCols[i % 2], cy: engRows[Math.floor(i / 2)] };
    });

    var devices = [
      { name: 'CPU', sub: 'x86 / ARM' },
      { name: 'CUDA', sub: 'NVIDIA GPU' },
      { name: 'Metal', sub: 'Apple GPU' },
      { name: 'NPU', sub: 'Ryzen AI' },
      { name: 'ROCm', sub: 'AMD GPU' },
      { name: 'Vulkan', sub: 'any GPU' }
    ];
    var devCols = [133, 310, 487], devRows = [368, 430];
    var devPos = {};
    devices.forEach(function (d, i) {
      devPos[d.name] = { cx: devCols[i % 3], cy: devRows[Math.floor(i / 3)] };
    });

    function engChip(name) {
      var p = engPos[name];
      return '<g class="hp-stack-chip"><rect x="' + (p.cx - 124) + '" y="' + (p.cy - 16) + '" width="248" height="32" rx="8"></rect>' +
        '<text class="hp-stack-chip-name" x="' + p.cx + '" y="' + (p.cy + 4) + '" text-anchor="middle">' + escapeText(name) + '</text></g>';
    }
    function devChip(d) {
      var p = devPos[d.name];
      return '<g class="hp-stack-chip"><rect x="' + (p.cx - 80) + '" y="' + (p.cy - 23) + '" width="160" height="46" rx="9"></rect>' +
        '<text class="hp-stack-chip-name" x="' + p.cx + '" y="' + (p.cy - 4) + '" text-anchor="middle">' + escapeText(d.name) + '</text>' +
        '<text class="hp-stack-chip-sub" x="' + p.cx + '" y="' + (p.cy + 15) + '" text-anchor="middle">' + escapeText(d.sub) + '</text></g>';
    }
    var engChips = engines.map(engChip).join('');
    var devChips = devices.map(devChip).join('');

    // Three requests, one per beat. Each beat is a full round-trip from "your
    // code", routed in straight vertical/horizontal legs (never diagonally, so
    // it's easy to track): the request dot drops down the centre spine beside the
    // chosen engine (lighting it), continues down into the device row, slides
    // horizontally into the device, which "executes" (an expanding ripple); then
    // the dot turns from request-yellow to response-green, slides back to centre
    // and rises straight up into the code. One engine + one device per beat.
    var beats = [
      { eng: 'whisper.cpp', dev: 'NPU' },
      { eng: 'llama.cpp', dev: 'ROCm' },
      { eng: 'stable-diffusion.cpp', dev: 'Vulkan' }
    ];
    var slice = 1 / beats.length;
    var codeY = 44;   // the request leaves "your code" here, just below the title bar

    // Request (warm lemon) vs response (fresh green) palette for the dot.
    var REQ_FILL = '#fff8c2', REQ_STROKE = 'rgba(252,216,70,0.92)';
    var RES_FILL = '#bfffd8', RES_STROKE = 'rgba(72,212,140,0.95)';

    var glows = '', wires = '', execs = '', dots = '';
    beats.forEach(function (b, i) {
      var e = engPos[b.eng], d = devPos[b.dev];
      var s = i * slice;
      // True only when the device sits off the centre spine, so the trip needs a
      // horizontal leg (and a device-row corner). When it's on the spine the dot
      // just drops straight in and out with no corner.
      var bend = d.cx !== appCx;
      // Beat timeline (fractions of the full cycle): launch -> reach the engine
      // (tTouch = tendril hits its edge) -> pause beside it (tEng..tEngHold) ->
      // [corner] -> into the device, execute, then [corner] -> straight up home.
      var tLaunch = s + 0.015, tEng = s + 0.055, tTouch = s + 0.07, tEngHold = s + 0.085,
          tCorner = s + 0.115, tDev = s + 0.15, tExec = s + 0.225, tRetCorner = s + 0.265,
          tBack = s + 0.305, tEnd = s + 0.32;

      // Engine lights the instant the wire tendril touches its edge (tTouch) and
      // stays lit until the response is home. (keyTimes stay <= 1 for every beat,
      // so the last beat lights too.)
      glows += '<g class="hp-stack-glow"><rect x="' + (e.cx - 124) + '" y="' + (e.cy - 16) + '" width="248" height="32" rx="8" opacity="0">' +
          ranim('opacity', '0;0;0.9;0.9;0', '0;' + f(tTouch - 0.006) + ';' + f(tTouch) + ';' + f(tBack) + ';' + f(tEnd)) +
        '</rect></g>';
      // Device lights only while it executes.
      glows += '<g class="hp-stack-glow"><rect x="' + (d.cx - 80) + '" y="' + (d.cy - 23) + '" width="160" height="46" rx="9" opacity="0">' +
          ranim('opacity', '0;0;0.9;0.9;0', '0;' + f(tDev - 0.02) + ';' + f(tDev) + ';' + f(tExec) + ';' + f(tExec + 0.03)) +
        '</rect></g>';

      // Glowing trace the dot leaves behind: code -> engine -> device. pathLength
      // is normalised to 1 so the dash "draws" in lockstep with the dot (offset 1
      // = hidden, 0 = fully drawn), then the whole trace fades as the dot returns.
      var L1 = Math.abs(e.cy - codeY), L2 = Math.abs(d.cy - e.cy), L3 = Math.abs(d.cx - appCx);
      var tot = L1 + L2 + L3 || 1, f1 = L1 / tot, f2 = (L1 + L2) / tot;
      var wirePath = 'M ' + appCx + ' ' + codeY + ' L ' + appCx + ' ' + e.cy +
        ' L ' + appCx + ' ' + d.cy + (bend ? ' L ' + d.cx + ' ' + d.cy : '');
      var doVals = ['1', '1', f(1 - f1), f(1 - f1)];   // hold at the engine row during the pause
      var doTimes = ['0', f(tLaunch), f(tEng), f(tEngHold)];
      if (bend) { doVals.push(f(1 - f2)); doTimes.push(f(tCorner)); }
      doVals.push('0'); doTimes.push(f(tDev));
      wires += '<path class="hp-stack-trace" d="' + wirePath + '" pathLength="1" ' +
          'stroke-dasharray="1 1" stroke-dashoffset="1" opacity="0">' +
          ranim('stroke-dashoffset', doVals.join(';'), doTimes.join(';')) +
          ranim('opacity', '0;0;0.8;0.8;0', '0;' + f(tLaunch) + ';' + f(tEng) + ';' + f(tExec) + ';' + f(tBack)) +
        '</path>';

      // A short tendril reaches off the spine to the NEAR edge of the chosen
      // engine; it draws (tEng -> tTouch) just as the dot passes that row, and the
      // engine lights the moment the tendril lands on its edge (tTouch).
      var engEdgeX = e.cx + (e.cx > appCx ? -124 : 124);
      wires += '<path class="hp-stack-trace" d="M ' + appCx + ' ' + e.cy + ' L ' + engEdgeX + ' ' + e.cy + '" ' +
          'pathLength="1" stroke-dasharray="1 1" stroke-dashoffset="1" opacity="0">' +
          ranim('stroke-dashoffset', '1;1;0', '0;' + f(tEng) + ';' + f(tTouch)) +
          ranim('opacity', '0;0;0.8;0.8;0', '0;' + f(tEng) + ';' + f(tTouch) + ';' + f(tExec) + ';' + f(tBack)) +
        '</path>';

      // "Execute" = two staggered rings rippling out of the device chip.
      [0, 0.035].forEach(function (delay) {
        var rs = tDev + delay, re = tExec + delay, rm = (rs + re) / 2;
        execs += '<circle class="hp-stack-exec-ring" cx="' + d.cx + '" cy="' + d.cy + '" r="8" opacity="0">' +
            ranim('r', '8;8;52', '0;' + f(rs) + ';' + f(re)) +
            ranim('opacity', '0;0;0.6;0', '0;' + f(rs) + ';' + f(rm) + ';' + f(re)) +
          '</circle>';
      });

      // The request/response dot. cx/cy are driven directly with LINEAR motion, so
      // every leg is purely vertical or horizontal and the dot turns each corner
      // at speed (no easing pause). Waypoints: code -> down past the engine ->
      // [corner] -> into the device (dwell = execute) -> [corner] -> straight up
      // home. fill/stroke flip to the response palette as the device finishes.
      var T = ['0'], X = [appCx], Y = [codeY], O = ['0'];
      function wp(t, x, y, o) { T.push(f(t)); X.push(x); Y.push(y); O.push(o); }
      wp(tLaunch, appCx, codeY, '0.95');
      wp(tEng, appCx, e.cy, '0.95');
      wp(tEngHold, appCx, e.cy, '0.95');     // momentary pause beside the engine
      if (bend) wp(tCorner, appCx, d.cy, '0.95');
      wp(tDev, d.cx, d.cy, '0.95');
      wp(tExec, d.cx, d.cy, '0.95');        // dwell at the device = execute
      if (bend) wp(tRetCorner, appCx, d.cy, '0.95');
      wp(tBack, appCx, codeY, '0.95');
      wp(tEnd, appCx, codeY, '0');
      wp(1, appCx, codeY, '0');
      dots += '<circle class="hp-stack-dot" r="5" cx="' + appCx + '" cy="' + codeY + '" ' +
          'fill="' + REQ_FILL + '" stroke="' + REQ_STROKE + '" opacity="0">' +
          ranim('cx', X.join(';'), T.join(';')) +
          ranim('cy', Y.join(';'), T.join(';')) +
          ranim('opacity', O.join(';'), T.join(';')) +
          ranim('fill', REQ_FILL + ';' + REQ_FILL + ';' + RES_FILL + ';' + RES_FILL, '0;' + f(tDev) + ';' + f(tExec) + ';1') +
          ranim('stroke', REQ_STROKE + ';' + REQ_STROKE + ';' + RES_STROKE + ';' + RES_STROKE, '0;' + f(tDev) + ';' + f(tExec) + ';1') +
        '</circle>';
    });

    // lemond is EMBEDDED in the developer's app, so the whole slide IS the app:
    // the standard dark app-window chrome (titled "Your App", like the spawn /
    // private-stack demos) wraps the body, and the lemond module (engines over
    // devices) sits inside it. The dev's own code calls into it across the
    // OpenAI-compatible API. Nothing lives "outside" the app; lemond ships with it.
    return '<div class="hp-app-window is-dark hp-stack-term">' +
      '<div class="hp-app-window-bar">' +
        '<span class="hp-app-window-title">Your App</span>' +
        '<span class="hp-app-window-dots"><i></i><i></i><i></i></span>' +
      '</div>' +
      '<svg class="hp-stack-svg" viewBox="0 0 620 500" preserveAspectRatio="xMidYMid meet" aria-hidden="true" focusable="false">' +
        '<defs>' + routerGlowFilter() + '</defs>' +
        // the dev's own code, calling lemond across its API
        '<g class="hp-stack-code"><rect x="244" y="14" width="132" height="30" rx="8"></rect>' +
          '<text class="hp-stack-codelabel" x="310" y="33" text-anchor="middle">your code</text></g>' +
        '<path class="hp-stack-wire" d="M 310 44 L 310 64"></path>' +
        // lemond module, embedded INSIDE the app.
        '<rect class="hp-stack-slab" x="22" y="64" width="576" height="414" rx="16"></rect>' +
        lemonMark(64, 92, 11) +
        '<text class="hp-stack-wordmark" x="84" y="88">lemond</text>' +
        '<text class="hp-stack-apisub" x="84" y="103">OpenAI-compatible API</text>' +
        '<text class="hp-stack-bandlabel" x="53" y="132">ENGINES &middot; managed for you</text>' +
        '<text class="hp-stack-bandlabel" x="53" y="334">DEVICES &middot; auto-selected</text>' +
        engChips + devChips +
        glows + wires + execs + dots +
      '</svg>' +
    '</div>';
  }

  function routerDemo(kind) {
    if (kind === 'router-hybrid') {
      // Hybrid = right TIER: requests stay local by default (free, private), and
      // escalate to your on-prem server (more power, still private) or the
      // frontier cloud (max intelligence) only when the task demands it. The
      // three-column stage (prompt -> tier -> response) fans straight from the
      // prompt to the chosen tier. Tiers are stacked by ascending intelligence
      // (cloud on top); the local route fires first so the default reads before
      // the escalations.
      return routerDiagram({
        id: 'hpHybridRoute',
        className: 'hp-router-hybrid-demo',
        request: 'Prompt',
        // viewBox aspect matches the 600x420 slide stage (1.43) so it fills with
        // no letterbox; the extra height lets the three tier cards breathe.
        // Prompt/response are inset from the edges and the tiers sit well clear
        // of the response. Slow `flow`/`cadence` so each scenario is readable.
        viewW: 780, viewH: 546, centerY: 273,
        requestX: 116, responseX: 666,
        flow: { requestHold: 420, travelHalf: 430, modelHold: 520, responseHold: 1750 },
        cadence: { subsectionDelay: 3900, subsectionGap: 380 },
        // Request typography: sized + line-spaced + wrapped to sit comfortably
        // inside the 150-wide prompt pill (no italic, no cramped lines).
        art: { promptFont: 17, promptWrap: 13, promptLh: 24 },
        targets: [
          { label: 'Frontier cloud', y: 122, x: 392, w: 256, h: 96, icon: 'cloud', sub: 'Max intelligence' },
          { label: 'On-prem server', y: 273, x: 392, w: 256, h: 96, icon: 'server', sub: 'More power · Private' },
          { label: 'Local', y: 424, x: 392, w: 256, h: 96, icon: 'device', sub: 'Free · Private', badge: 'DEFAULT' }
        ],
        routes: [
          {
            target: 2,
            request: { type: 'prompt', text: 'Summarize my meeting notes' },
            response: { type: 'summary', label: 'Action items' }
          },
          {
            target: 1,
            request: { type: 'prompt', text: 'Analyze this contract for red flags' },
            response: { type: 'flags', label: 'Contract review' }
          },
          {
            target: 0,
            request: { type: 'prompt', text: 'Plan a major refactor of this codebase' },
            response: { type: 'glyph', glyph: '</>', label: 'Refactor plan' }
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
      if (kind === 'backend-stack') return stackDemo();
      return routerDemo(kind);
    },
    // Exposed so the persona-demo framework (loaded after this module) can reuse
    // one HTML-escaper instead of maintaining a second copy.
    escapeText: escapeText
  };
})();
