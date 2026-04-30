// Shared Navbar Component for Lemonade docs pages

function createNavbar(basePath = '') {
  return `
    <nav class="navbar" id="navbar">
      <div class="navbar-brand">
        <span class="brand-title"><a href="https://lemonade-server.ai"><img class="brand-icon" src="${basePath}favicon.ico" alt="" />Lemonade</a></span>
      </div>
      <div class="navbar-links" id="navbarLinks">
        <a href="${basePath}docs/">Docs</a>
        <a href="${basePath}models.html">Models</a>
        <a href="${basePath}marketplace.html">Marketplace</a>
        <a href="https://github.com/lemonade-sdk/lemonade" target="_blank" rel="noopener">GitHub</a>
        <a href="${basePath}news/">News</a>
      </div>
      <div class="navbar-actions">
        <a class="navbar-install-btn" href="${basePath}index.html#getting-started">Get started</a>
        <button class="navbar-toggle" id="navbarToggle" type="button" aria-label="Toggle navigation menu" aria-expanded="false" aria-controls="navbarLinks">
          <span class="navbar-toggle-bar"></span>
          <span class="navbar-toggle-bar"></span>
          <span class="navbar-toggle-bar"></span>
        </button>
      </div>
    </nav>
  `;
}

function initializeNavbar(basePath = '') {
  const navbarContainer = document.querySelector('.navbar-placeholder');
  if (!navbarContainer) {
    console.warn('Navbar placeholder not found');
    return;
  }
  navbarContainer.innerHTML = createNavbar(basePath);

  const toggle = document.getElementById('navbarToggle');
  const links = document.getElementById('navbarLinks');
  if (!toggle || !links) return;

  function setOpen(open) {
    links.classList.toggle('is-open', open);
    toggle.classList.toggle('is-open', open);
    toggle.setAttribute('aria-expanded', open ? 'true' : 'false');
  }

  toggle.addEventListener('click', function() {
    setOpen(!links.classList.contains('is-open'));
  });
  links.addEventListener('click', function(e) {
    if (e.target.tagName === 'A') setOpen(false);
  });
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { createNavbar, initializeNavbar };
}
