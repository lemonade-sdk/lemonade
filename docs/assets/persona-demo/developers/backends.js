// Developers · section 3 · "One stack, every model"
// Two flowchart demos that each land a value prop:
//   1. The stack       -- lemond is the abstraction layer over engines + devices.
//   2. Expanding models -- engines keep shipping; new models appear in your app.
// Both are pure-SVG + SMIL diagrams rendered by the shared flowchart engine.
(function () {
  var P = window.LemonadePersona;
  if (!P) return;
  var h = P.helpers;

  P.registerSection('developers', 3, {
    title: 'One stack, every model',
    slides: [
      {
        label: 'Engines & hardware, handled for you',
        demo: 'backend-stack',
        caption: 'Your app calls one API — lemond runs the engines and picks the hardware for you.',
        captionHref: 'https://lemonade-server.ai/docs/embeddable/backends/',
        animationMode: 'repeat'
      },
      {
        label: 'Ever-expanding models',
        demo: 'backend-models',
        caption: 'Engines keep shipping. New models show up in your app automatically.',
        captionHref: 'models.html',
        animationMode: 'repeat'
      }
    ]
  });

  P.registerDemo('backend-stack', function (frame) { h.renderFlowchart(frame, 'backend-stack'); });
  P.registerDemo('backend-models', function (frame) { h.renderFlowchart(frame, 'backend-models'); });
})();
