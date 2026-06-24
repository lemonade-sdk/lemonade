// Developers · section 1 · "Smart Router"
// Slides: omni virtual models + cloud/local hybrid routing (both flowcharts).
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
        captionHref: 'https://lemonade-server.ai/docs/dev/omni-router/',
        animationMode: 'repeat'
      },
      {
        label: 'Cloud/local hybrid',
        demo: 'router-hybrid',
        caption: 'Cloud models when needed, local by default.',
        captionHref: 'https://lemonade-server.ai/docs/guide/configuration/cloud/',
        animationMode: 'repeat'
      }
    ]
  });

  P.registerDemo('router-omni', function(frame) { h.renderFlowchart(frame, 'router-omni'); });
  P.registerDemo('router-hybrid', function(frame) { h.renderFlowchart(frame, 'router-hybrid'); });
})();
