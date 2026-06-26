// Developers · section 1 · "Smart Router"
// Slides: omni virtual models, cloud-and-server tier routing, and the
// engines/hardware abstraction (all flowcharts).
(function () {
  var P = window.LemonadePersona;
  if (!P) return;
  var h = P.helpers;

  P.registerSection('developers', 1, {
    title: 'Smart Router',
    slides: [
      {
        label: 'Omni models',
        demo: 'router-omni',
        caption: 'Send and receive multimedia with virtual omni models.',
        captionHref: 'https://lemonade-server.ai/docs/dev/lemonade-omni/',
        animationMode: 'repeat'
      },
      {
        label: 'Cloud and server routing',
        demo: 'router-hybrid',
        caption: 'A smart router keeps every request local by default — escalating to your server or the cloud only when the task demands it.',
        captionHref: 'https://lemonade-server.ai/docs/guide/configuration/cloud/',
        animationMode: 'repeat'
      },
      {
        label: 'Engines and hardware',
        demo: 'backend-stack',
        caption: 'Your app calls one API — lemond runs the engines and picks the hardware for you.',
        captionHref: 'https://lemonade-server.ai/docs/embeddable/backends/',
        animationMode: 'repeat'
      }
    ]
  });

  P.registerDemo('router-omni', function(frame) { h.renderFlowchart(frame, 'router-omni'); });
  P.registerDemo('router-hybrid', function(frame) { h.renderFlowchart(frame, 'router-hybrid'); });
  P.registerDemo('backend-stack', function(frame) { h.renderFlowchart(frame, 'backend-stack'); });
})();
