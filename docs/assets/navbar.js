// Shared Navbar Component for Lemonade docs pages

function createNavbar(basePath = '') {
  return `
    <nav class="navbar" id="navbar">
      <div class="navbar-brand">
        <span class="brand-title"><a href="https://lemonade-server.ai"><img class="brand-icon" src="${basePath}favicon.ico" alt="" />Lemonade</a></span>
      </div>
      <div class="navbar-links">
        <a href="${basePath}docs/">Docs</a>
        <a href="${basePath}models.html">Models</a>
        <a href="${basePath}marketplace.html">Marketplace</a>
        <a href="https://github.com/lemonade-sdk/lemonade" target="_blank" rel="noopener">GitHub</a>
        <a href="${basePath}news/">News</a>
      </div>
      <div class="navbar-actions">
        <a class="navbar-install-btn" href="${basePath}index.html#getting-started">Get started</a>
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
