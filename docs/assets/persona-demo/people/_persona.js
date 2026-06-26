// People persona: header + section ordering. The hero + promise are static in
// index.html; only the zone heading + journey are persona-aware. stepIcons are
// the TOC section badges (developers use numeric badges instead).
(function () {
  var P = window.LemonadePersona;
  if (!P) return;
  P.registerPersona('people', {
    zone: 'Get to know Lemonade.',
    zoneSubtitle: 'From your first chat to your own self-hosted server, see everything you can do with local AI on your hardware.',
    label: 'User journey',
    stepIcons: ['explore', 'apps', 'developer_board', 'terminal', 'dns']
  });
})();
