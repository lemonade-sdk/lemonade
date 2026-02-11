// Shared Navbar Component for Lemonade docs pages

function createNavbar(basePath = '') {
  return `
    <nav class="navbar" id="navbar">
      <div class="navbar-brand">
        <span class="brand-title"><a href="https://lemonade-server.ai">🍋 Lemonade</a></span>
      </div>
      <div class="navbar-links">
        <a href="https://github.com/lemonade-sdk/lemonade">GitHub</a>
        <a href="https://lemonade-server.ai/docs/">Docs</a>
        <a href="${basePath}models.html">Models</a>
        <a href="${basePath}marketplace.html">Marketplace</a>
        <a href="${basePath}news/">News</a>
      </div>
    </nav>
  `;
}

function initializeNavbar(basePath = '') {
  const navbarContainer = document.querySelector('.navbar-placeholder');
  if (navbarContainer) {
    navbarContainer.innerHTML = createNavbar(basePath);
  } else {
    console.warn('Navbar placeholder not found');
  }
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { createNavbar, initializeNavbar };
}
