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
        <a href="${basePath}news/">News</a>
      </div>
      <div class="navbar-actions">
        <a class="navbar-github-btn" href="https://github.com/lemonade-sdk/lemonade" target="_blank" rel="noopener">
          <svg class="navbar-github-icon" viewBox="0 0 16 16" fill="currentColor"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.01 8.01 0 0 0 16 8c0-4.42-3.58-8-8-8z"/></svg>
          GitHub
        </a>
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
