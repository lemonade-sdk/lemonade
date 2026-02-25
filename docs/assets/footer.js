// Shared Footer Component for Lemonade Website
// This ensures consistent footer across all pages

function createFooter(basePath = '') {
  return `
    <footer class="site-footer">
      <div class="footer-content">
        <nav class="footer-nav-rail" role="navigation" aria-label="Footer quick links">
          <a href="https://github.com/lemonade-sdk/lemonade" class="footer-rail-link footer-rail-link--community" target="_blank" rel="noopener">
            <span class="footer-rail-icon" aria-hidden="true">
              <img src="https://cdn.simpleicons.org/github/1f2937" alt="" loading="lazy" decoding="async" />
            </span>
            <span>GitHub</span>
          </a>
          <a href="https://discord.gg/5xXzkMu8Zk" class="footer-rail-link footer-rail-link--community" target="_blank" rel="noopener">
            <span class="footer-rail-icon" aria-hidden="true">
              <img src="https://cdn.simpleicons.org/discord/5865F2" alt="" loading="lazy" decoding="async" />
            </span>
            <span>Discord</span>
          </a>
          <a href="${basePath}docs/dev-getting-started/" class="footer-rail-link footer-rail-link--product">Developer Setup</a>
          <a href="${basePath}docs/models.html" class="footer-rail-link footer-rail-link--product">Models</a>
          <a href="${basePath}marketplace.html" class="footer-rail-link footer-rail-link--product">Marketplace</a>
          <a href="${basePath}docs/" class="footer-rail-link footer-rail-link--product">Documentation</a>
        </nav>
        <div class="footer-meta">
          <div class="copyright">© 2026 AMD. Licensed under Apache 2.0.</div>
        </div>
      </div>
    </footer>
  `;
}

// Function to initialize footer on page load
function initializeFooter(basePath = '') {
  const footerContainer = document.querySelector('.footer-placeholder');
  if (footerContainer) {
    footerContainer.innerHTML = createFooter(basePath);
  } else {
    console.warn('Footer placeholder not found');
  }
}

// Function to initialize footer with DOMContentLoaded wrapper (for standalone use)
function initializeFooterOnLoad(basePath = '') {
  document.addEventListener('DOMContentLoaded', function() {
    initializeFooter(basePath);
  });
}

// Function to initialize footer when footer is already in DOM
function initializeFooterStarCount() {
  // No-op kept for backward compatibility with older pages/scripts.
}

// Export for use in other scripts
if (typeof module !== 'undefined' && module.exports) {
  module.exports = { createFooter, initializeFooter, initializeFooterOnLoad, initializeFooterStarCount };
}
