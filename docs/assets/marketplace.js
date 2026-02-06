/**
 * Marketplace JavaScript
 * Handles loading, filtering, and displaying apps
 */

// Configuration
const APPS_JSON_URL = 'https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps.json';

// State
let allApps = [];
let categories = [];
let selectedCategory = 'all';
let searchQuery = '';

/**
 * Check if running in embedded mode (inside Electron app)
 */
function isEmbedded() {
  const params = new URLSearchParams(window.location.search);
  return params.get('embedded') === 'true';
}

/**
 * Check if dark theme is requested
 */
function isDarkTheme() {
  const params = new URLSearchParams(window.location.search);
  return params.get('theme') === 'dark';
}

/**
 * Initialize the page
 */
function initMarketplace() {
  // Apply embedded mode
  if (isEmbedded()) {
    document.documentElement.classList.add('embedded');
    document.body.classList.add('embedded');
  }

  // Apply dark theme
  if (isDarkTheme()) {
    document.documentElement.setAttribute('data-theme', 'dark');
  }

  // Setup search
  const searchInput = document.getElementById('search-input');
  if (searchInput) {
    searchInput.addEventListener('input', debounce(handleSearch, 200));
  }
}

/**
 * Load apps from the JSON file
 */
async function loadApps() {
  const grid = document.getElementById('apps-grid');
  const loading = document.getElementById('loading-state');
  const error = document.getElementById('error-state');
  const empty = document.getElementById('empty-state');

  // Show loading
  grid.classList.add('hidden');
  loading.classList.add('show');
  error.classList.remove('show');
  empty.classList.remove('show');

  try {
    const response = await fetch(APPS_JSON_URL);
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    const data = await response.json();
    allApps = data.apps || [];
    categories = data.categories || [];

    // Render categories
    renderCategories();

    // Render apps
    renderApps();

    // Hide loading
    loading.classList.remove('show');
    grid.classList.remove('hidden');

  } catch (err) {
    console.error('Failed to load apps:', err);
    loading.classList.remove('show');
    error.classList.add('show');
  }
}

/**
 * Render category filter buttons or dropdown (for embedded mode)
 */
function renderCategories() {
  const container = document.getElementById('category-filters');
  if (!container) return;

  // In embedded mode, use a dropdown for space efficiency
  if (isEmbedded()) {
    let options = `<option value="all">All Categories</option>`;
    for (const cat of categories) {
      options += `<option value="${cat.id}">${cat.label}</option>`;
    }

    container.innerHTML = `
      <select class="mp-filter-select" id="category-select">
        ${options}
      </select>
    `;

    const select = document.getElementById('category-select');
    if (select) {
      select.addEventListener('change', (e) => {
        selectedCategory = e.target.value;
        renderApps();
      });
    }
    return;
  }

  // Non-embedded: use buttons
  let html = `<button class="mp-filter-btn active" data-category="all">All</button>`;

  // Create category buttons
  for (const cat of categories) {
    html += `<button class="mp-filter-btn" data-category="${cat.id}">${cat.label}</button>`;
  }

  container.innerHTML = html;

  // Add click handlers
  container.querySelectorAll('.mp-filter-btn').forEach(btn => {
    btn.addEventListener('click', () => handleCategoryClick(btn));
  });
}

/**
 * Handle category button click
 */
function handleCategoryClick(btn) {
  // Update active state
  document.querySelectorAll('.mp-filter-btn').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');

  // Update selected category
  selectedCategory = btn.dataset.category;

  // Re-render apps
  renderApps();
}

/**
 * Handle search input
 */
function handleSearch(e) {
  searchQuery = e.target.value.toLowerCase().trim();
  renderApps();
}

/**
 * Filter apps based on current state
 */
function filterApps() {
  return allApps.filter(app => {
    // Category filter
    if (selectedCategory !== 'all') {
      if (!app.category || !app.category.includes(selectedCategory)) {
        return false;
      }
    }

    // Search filter
    if (searchQuery) {
      const nameMatch = app.name.toLowerCase().includes(searchQuery);
      const descMatch = app.description.toLowerCase().includes(searchQuery);
      if (!nameMatch && !descMatch) {
        return false;
      }
    }

    return true;
  });
}

/**
 * Render app cards
 */
function renderApps() {
  const grid = document.getElementById('apps-grid');
  const empty = document.getElementById('empty-state');

  const filteredApps = filterApps();

  if (filteredApps.length === 0) {
    grid.classList.add('hidden');
    empty.classList.add('show');
    return;
  }

  grid.classList.remove('hidden');
  empty.classList.remove('show');

  let html = '';
  for (const app of filteredApps) {
    html += renderAppCard(app);
  }

  grid.innerHTML = html;

  // Prevent card click when clicking links
  grid.querySelectorAll('.app-link').forEach(link => {
    link.addEventListener('click', e => e.stopPropagation());
  });

  // Make cards clickable
  grid.querySelectorAll('.app-card').forEach(card => {
    card.addEventListener('click', () => {
      const url = card.dataset.url;
      if (url) {
        window.open(url, '_blank', 'noopener,noreferrer');
      }
    });
  });
}

/**
 * Render a single app card
 */
function renderAppCard(app) {
  const categoryTags = (app.category || [])
    .slice(0, 2)
    .map(cat => `<span class="category-tag">${cat}</span>`)
    .join('');

  // No featured badge - keeping it clean

  // Build links
  let linksHtml = '';
  if (app.links) {
    if (app.links.app) {
      linksHtml += `<a href="${app.links.app}" class="app-link primary" target="_blank" rel="noopener noreferrer">Visit</a>`;
    }
    if (app.links.guide) {
      linksHtml += `<a href="${app.links.guide}" class="app-link secondary" target="_blank" rel="noopener noreferrer">Guide</a>`;
    }
    if (app.links.video) {
      linksHtml += `<a href="${app.links.video}" class="app-link secondary" target="_blank" rel="noopener noreferrer">Video</a>`;
    }
  }

  const logoUrl = app.logo || 'data:image/svg+xml,<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"><rect fill="%23ddd" width="100" height="100"/><text x="50" y="55" text-anchor="middle" font-size="40" fill="%23999">?</text></svg>';

  return `
    <div class="app-card" data-url="${app.links?.app || ''}">
      <div class="app-card-header">
        <img src="${logoUrl}" alt="${app.name}" class="app-logo" loading="lazy" onerror="this.src='data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 100 100%22><rect fill=%22%23ddd%22 width=%22100%22 height=%22100%22/><text x=%2250%22 y=%2255%22 text-anchor=%22middle%22 font-size=%2240%22 fill=%22%23999%22>?</text></svg>'">
        <div class="app-info">
          <h3 class="app-name">${escapeHtml(app.name)}</h3>
          <div class="app-categories">${categoryTags}</div>
        </div>
      </div>
      <p class="app-description">${escapeHtml(app.description)}</p>
      <div class="app-links">${linksHtml}</div>
    </div>
  `;
}

/**
 * Escape HTML to prevent XSS
 */
function escapeHtml(text) {
  const div = document.createElement('div');
  div.textContent = text;
  return div.innerHTML;
}

/**
 * Debounce function for search
 */
function debounce(func, wait) {
  let timeout;
  return function executedFunction(...args) {
    const later = () => {
      clearTimeout(timeout);
      func(...args);
    };
    clearTimeout(timeout);
    timeout = setTimeout(later, wait);
  };
}

// Initialize when DOM is ready
document.addEventListener('DOMContentLoaded', initMarketplace);
