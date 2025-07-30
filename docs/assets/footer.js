// Shared Footer Component for Lemonade Website
// This ensures consistent footer across all pages

// Fetch GitHub repository star count
async function fetchStarCount() {
  try {
    const response = await fetch('https://api.github.com/repos/lemonade-sdk/lemonade');
    const repo = await response.json();
    
    if (repo.stargazers_count !== undefined) {
      const starCount = repo.stargazers_count;
      const starElement = document.getElementById('star-count');
      if (starElement) {
        // Format star count with k/M abbreviations for large numbers
        let displayCount = starCount;
        if (starCount >= 1000000) {
          displayCount = (starCount / 1000000).toFixed(1) + 'M';
        } else if (starCount >= 1000) {
          displayCount = (starCount / 1000).toFixed(1) + 'k';
        }
        starElement.textContent = `${displayCount} Stars`;
      }
    }
  } catch (error) {
    // Silently fallback to just "Stars" if API call fails
    // This prevents console noise in production
  }
}

function createFooter(basePath = '') {
  return `
    <footer class="site-footer">
      <div class="footer-content">
        <div class="footer-brand">
          <h3>🍋 Lemonade</h3>
          <div class="dad-joke">When life gives you LLMs, make an LLM aide.</div>
          <div class="footer-badges">
            <a href="https://github.com/lemonade-sdk/lemonade" class="footer-badge subtle" id="stars-badge">
              <span class="star-icon">⭐</span>
              <span id="star-count">Stars</span>
            </a>
          </div>
        </div>
        
        <div class="footer-section">
          <h4>Product</h4>
          <div class="footer-links">
            <a href="https://github.com/lemonade-sdk/lemonade/releases/latest/download/Lemonade_Server_Installer.exe">Download</a>
            <a href="${basePath}docs/">Documentation</a>
            <a href="${basePath}docs/server/server_models/">Model Library</a>
            <a href="${basePath}docs/lemonade_api/">API Reference</a>
          </div>
        </div>
        
        <div class="footer-section">
          <h4>Community</h4>
          <div class="footer-links">
            <a href="https://discord.gg/5xXzkMu8Zk">
              Discord
            </a>
            <a href="https://github.com/lemonade-sdk/lemonade">
              GitHub
            </a>
            <a href="https://github.com/lemonade-sdk/lemonade/issues">
              Issues
            </a>
            <a href="mailto:lemonade@amd.com">
              Email
            </a>
          </div>
        </div>
        
        <div class="footer-section">
          <h4>Resources</h4>
          <div class="footer-links">
            <a href="${basePath}docs/">Getting Started</a>
            <a href="${basePath}docs/server/server_integration/">Integration Guide</a>
            <a href="${basePath}docs/contribute/">Contributing</a>
            <a href="https://github.com/lemonade-sdk/lemonade/issues?q=is%3Aissue+is%3Aopen+label%3A%22good+first+issue%22">Good First Issues</a>
          </div>
        </div>
      </div>
      
      <div class="footer-bottom">
        <div class="copyright">© 2025 AMD. Licensed under Apache 2.0.</div>
      </div>
    </footer>
  `;
}

// Function to initialize footer on page load
function initializeFooter(basePath = '') {
  const footerContainer = document.querySelector('.footer-placeholder');
  if (footerContainer) {
    footerContainer.innerHTML = createFooter(basePath);
    // Fetch star count after footer is inserted
    setTimeout(fetchStarCount, 100);
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
  fetchStarCount();
}

// Export for use in other scripts
if (typeof module !== 'undefined' && module.exports) {
  module.exports = { createFooter, initializeFooter, initializeFooterOnLoad, initializeFooterStarCount, fetchStarCount };
}
