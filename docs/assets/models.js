const GITHUB_REPO = 'lemonade-sdk/lemonade';
const TAGS_URL = `https://api.github.com/repos/${GITHUB_REPO}/tags?per_page=100`;
const RAW_BASE = 'https://raw.githubusercontent.com/lemonade-sdk/lemonade';

const RECIPE_PRIORITY = [
  'llamacpp',
  'ryzenai-llm',
  'flm',
  'whispercpp',
  'sd-cpp',
  'oga-hybrid',
  'oga-npu',
  'oga-cpu',
  'kokoro'
];

const RECIPE_DISPLAY_NAMES = {
  llamacpp: 'llama.cpp GPU',
  'ryzenai-llm': 'Ryzen AI SW NPU',
  flm: 'FastFlowLM NPU',
  whispercpp: 'whisper.cpp',
  'sd-cpp': 'stable-diffusion.cpp'
};

const state = {
  tag: null,
  sourceUrl: null,
  models: [],
  search: '',
  recipe: 'all',
  label: 'all'
};

function hasLabel(details, label) {
  const labels = Array.isArray(details.labels) ? details.labels : [];
  return labels.includes(label);
}

function escapeHtml(text) {
  const div = document.createElement('div');
  div.textContent = String(text);
  return div.innerHTML;
}

function toTitle(text) {
  return String(text)
    .replace(/_/g, ' ')
    .replace(/-/g, ' ')
    .replace(/\b\w/g, (ch) => ch.toUpperCase());
}

function slugify(text) {
  return String(text).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/(^-|-$)/g, '');
}

function getRecipeDisplayName(recipe) {
  if (!recipe || recipe === 'unknown') return 'Unknown Recipe';
  return RECIPE_DISPLAY_NAMES[recipe] || toTitle(recipe);
}

function parseCheckpoint(name, details) {
  const raw = details.checkpoint;
  if (typeof raw !== 'string') {
    return { repo: null, variant: null, display: '' };
  }
  if (name.endsWith('-FLM')) {
    return { repo: raw, variant: null, display: raw };
  }
  const split = raw.split(':');
  if (split.length > 1 && split[0].includes('/')) {
    return { repo: split[0], variant: split.slice(1).join(':'), display: raw };
  }
  if (raw.includes('/')) {
    return { repo: raw, variant: null, display: raw };
  }
  return { repo: null, variant: null, display: raw };
}

function modelMatchesCatalogFilters(model) {
  const { name, details } = model;
  if (!details.suggested) {
    return false;
  }
  const recipe = details.recipe || 'unknown';
  if (state.recipe !== 'all' && recipe !== state.recipe) {
    return false;
  }
  if (!state.search) {
    return true;
  }
  const labels = Array.isArray(details.labels) ? details.labels.join(' ') : '';
  const blob = `${name} ${details.checkpoint || ''} ${details.recipe || ''} ${labels}`.toLowerCase();
  return blob.includes(state.search);
}

function modelMatchesFilters(model) {
  if (!modelMatchesCatalogFilters(model)) {
    return false;
  }
  if (state.label !== 'all' && !hasLabel(model.details, state.label)) {
    return false;
  }
  return true;
}

function getRecipeValues() {
  const values = new Set();
  state.models.forEach((model) => {
    if (model.details && model.details.suggested) {
      values.add(model.details.recipe || 'unknown');
    }
  });
  return [...values].sort((a, b) => {
    const aIndex = RECIPE_PRIORITY.indexOf(a);
    const bIndex = RECIPE_PRIORITY.indexOf(b);
    if (aIndex >= 0 && bIndex >= 0) return aIndex - bIndex;
    if (aIndex >= 0) return -1;
    if (bIndex >= 0) return 1;
    return a.localeCompare(b);
  });
}

function getLabelValues() {
  const values = new Set(['hot']);
  state.models.forEach((model) => {
    if (model.details && model.details.suggested && Array.isArray(model.details.labels)) {
      model.details.labels.forEach((label) => values.add(label));
    }
  });
  return [...values].sort((a, b) => a.localeCompare(b));
}

function renderRecipeButtons() {
  const container = document.getElementById('recipe-filter-buttons');
  const recipes = getRecipeValues();
  const buttons = [{ key: 'all', title: 'All recipes' }, ...recipes.map((recipe) => ({ key: recipe, title: getRecipeDisplayName(recipe) }))];
  container.innerHTML = buttons
    .map((item) => `
      <button class="backend-filter-btn ${state.recipe === item.key ? 'active' : ''}" data-recipe="${escapeHtml(item.key)}">
        ${escapeHtml(item.title)}
      </button>
    `)
    .join('');
  container.querySelectorAll('.backend-filter-btn').forEach((button) => {
    button.addEventListener('click', () => {
      state.recipe = button.dataset.recipe;
      renderRecipeButtons();
      renderModels();
    });
  });
}

function renderLabelButtons() {
  const container = document.getElementById('label-filter-buttons');
  const labels = getLabelValues().filter((label) => label !== 'hot' && label !== 'all');
  const buttons = [
    { key: 'hot', title: '🔥 hot' },
    { key: 'all', title: 'All labels' },
    ...labels.map((label) => ({ key: label, title: label }))
  ];
  container.innerHTML = buttons
    .map((item) => `
      <button class="label-filter-btn ${state.label === item.key ? 'active' : ''}" data-label="${escapeHtml(item.key)}">
        ${escapeHtml(item.title)}
      </button>
    `)
    .join('');
  container.querySelectorAll('.label-filter-btn').forEach((button) => {
    button.addEventListener('click', () => {
      state.label = button.dataset.label;
      renderLabelButtons();
      renderModels();
    });
  });
}

function buildModelTableRows(name, details) {
  const rows = [];
  const checkpoint = parseCheckpoint(name, details);

  if (checkpoint.repo) {
    rows.push({
      key: 'Checkpoint',
      value: `<a href="https://huggingface.co/${escapeHtml(checkpoint.repo)}" target="_blank" rel="noopener">${escapeHtml(checkpoint.repo)}</a>`
    });
    if (checkpoint.variant) {
      rows.push({ key: 'GGUF Variant', value: escapeHtml(checkpoint.variant) });
    }
  } else if (details.checkpoint) {
    rows.push({ key: 'Checkpoint', value: escapeHtml(details.checkpoint) });
  }

  for (const [key, value] of Object.entries(details)) {
    if (['checkpoint', 'max_prompt_length', 'suggested', 'labels'].includes(key)) {
      continue;
    }
    if (key === 'image_defaults' && value && typeof value === 'object') {
      if (value.steps !== undefined) rows.push({ key: 'Default Steps', value: escapeHtml(value.steps) });
      if (value.cfg_scale !== undefined) rows.push({ key: 'Default CFG Scale', value: escapeHtml(value.cfg_scale) });
      if (value.width !== undefined && value.height !== undefined) {
        rows.push({ key: 'Default Size', value: `${escapeHtml(value.width)}x${escapeHtml(value.height)}` });
      }
      continue;
    }
    if (key === 'size') {
      rows.push({ key: 'Size (GB)', value: escapeHtml(value) });
      continue;
    }
    rows.push({ key: toTitle(key), value: escapeHtml(value) });
  }
  return rows;
}

function renderModelCard(model, options = {}) {
  const { compact = false } = options;
  const { name, details } = model;
  const labels = Array.isArray(details.labels) ? details.labels : [];
  const tableRows = buildModelTableRows(name, details);
  const pullCommand = `lemonade-server pull ${name}`;

  const badgeMarkup = [
    ...labels.map((label) => `<span class="badge">${escapeHtml(label)}</span>`)
  ].join('');

  const rowMarkup = tableRows
    .map((row) => `<tr><td>${escapeHtml(row.key)}</td><td>${row.value}</td></tr>`)
    .join('');

  return `
    <article class="model-card">
      <div class="model-card-head">
        <h4 class="model-name">${escapeHtml(name)}</h4>
      </div>
      <div class="badge-row">${badgeMarkup}</div>
      <div class="model-pull-wrap">
        <pre class="model-pull"><code>${escapeHtml(pullCommand)}</code></pre>
        <button class="model-pull-copy-btn" type="button" aria-label="Copy pull command" title="Copy pull command" data-copy-text="${escapeHtml(pullCommand)}">
          <svg viewBox="0 0 24 24" focusable="false" aria-hidden="true">
            <path d="M9 9a2 2 0 0 1 2-2h8a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2h-8a2 2 0 0 1-2-2V9Zm-6 4V5a2 2 0 0 1 2-2h8v2H5v8H3Z"></path>
          </svg>
        </button>
      </div>
      ${compact ? '' : `
      <div class="model-details">
        <table class="kv-table">
          ${rowMarkup}
        </table>
      </div>
      `}
    </article>
  `;
}

function wireSectionToggles() {
  document.querySelectorAll('.model-section-toggle').forEach((button) => {
    button.addEventListener('click', () => {
      const sectionId = button.getAttribute('data-section-id');
      const sectionEl = document.getElementById(sectionId);
      if (!sectionEl) return;
      const willCollapse = !sectionEl.classList.contains('is-collapsed');
      sectionEl.classList.toggle('is-collapsed', willCollapse);
      const chevron = button.querySelector('.model-section-chevron');
      if (chevron) {
        chevron.textContent = willCollapse ? '▸' : '▾';
      }
    });
  });
}

function recipeDisplayName(recipe) {
  return getRecipeDisplayName(recipe);
}

function labelDisplayName(label) {
  if (label === 'all') return 'All labels';
  if (label === 'hot') return 'Hot';
  return toTitle(label);
}

function wireModelCardCopyButtons() {
  document.querySelectorAll('.model-pull-copy-btn').forEach((button) => {
    button.addEventListener('click', async () => {
      const text = button.dataset.copyText || '';
      if (!text) return;
      try {
        await navigator.clipboard.writeText(text);
      } catch (error) {
        const helper = document.createElement('textarea');
        helper.value = text;
        document.body.appendChild(helper);
        helper.select();
        document.execCommand('copy');
        document.body.removeChild(helper);
      }
      button.classList.add('is-copied');
      button.setAttribute('aria-label', 'Pull command copied');
      button.setAttribute('title', 'Pull command copied');
      setTimeout(() => {
        button.classList.remove('is-copied');
        button.setAttribute('aria-label', 'Copy pull command');
        button.setAttribute('title', 'Copy pull command');
      }, 1200);
    });
  });
}

function renderModels() {
  const sectionsHost = document.getElementById('supported-model-sections');
  const loading = document.getElementById('models-loading');
  const error = document.getElementById('models-error');
  const empty = document.getElementById('models-empty');
  const context = document.getElementById('models-context');
  const hotPicksSection = document.getElementById('hot-picks-section');
  const hotPicksGrid = document.getElementById('hot-picks-grid');
  const hotPicksTitle = document.querySelector('.hot-picks-title');

  loading.classList.add('hidden');
  error.classList.add('hidden');

  const baseFiltered = state.models.filter(modelMatchesCatalogFilters);
  const filtered = state.label === 'all'
    ? baseFiltered
    : baseFiltered.filter((model) => hasLabel(model.details, state.label));
  const hotCandidates = baseFiltered.filter((model) => hasLabel(model.details, 'hot'));

  const countText = `${filtered.length} model${filtered.length === 1 ? '' : 's'}`;
  const showClear = state.recipe !== 'all' || state.label !== 'all' || !!state.search;
  const searchText = state.search ? `Search: "${state.search}"` : '';
  context.innerHTML = `
    <span class="models-context-prefix">Showing</span>
    <span class="models-context-pill">${escapeHtml(labelDisplayName(state.label))}</span>
    <span class="models-context-pill">${escapeHtml(state.recipe === 'all' ? 'All recipes' : getRecipeDisplayName(state.recipe))}</span>
    ${searchText ? `<span class="models-context-pill">${escapeHtml(searchText)}</span>` : ''}
    <span class="models-context-count">${escapeHtml(countText)}</span>
    ${showClear ? '<button id="models-clear-filters-btn" class="models-clear-filters-btn" type="button">Clear</button>' : ''}
  `;
  context.classList.remove('hidden');

  if (state.label !== 'hot' && hotCandidates.length > 0) {
    const hotPicks = hotCandidates.slice(0, 6);
    hotPicksTitle.textContent = 'Hot models';
    hotPicksGrid.innerHTML = hotPicks.map((model) => renderModelCard(model, { compact: true })).join('');
    hotPicksSection.classList.remove('hidden');
  } else {
    hotPicksGrid.innerHTML = '';
    hotPicksSection.classList.add('hidden');
  }

  if (filtered.length === 0) {
    sectionsHost.classList.add('hidden');
    sectionsHost.innerHTML = '';
    empty.classList.remove('hidden');
    wireModelCardCopyButtons();
    return;
  }

  empty.classList.add('hidden');
  sectionsHost.classList.remove('hidden');

  const groupedByRecipe = new Map();
  filtered.forEach((model) => {
    const key = model.details.recipe || 'unknown';
    if (!groupedByRecipe.has(key)) groupedByRecipe.set(key, []);
    groupedByRecipe.get(key).push(model);
  });

  const orderedRecipes = getRecipeValues().filter((recipe) => groupedByRecipe.has(recipe));
  if (groupedByRecipe.has('unknown') && !orderedRecipes.includes('unknown')) {
    orderedRecipes.push('unknown');
  }

  const sectionHtml = [];
  orderedRecipes.forEach((recipe) => {
    const models = groupedByRecipe.get(recipe);
    const cards = models.map(renderModelCard).join('');
    const sectionId = `section-recipe-${slugify(recipe)}`;
    const isCollapsedByDefault = false;
    const modelCount = `${models.length} model${models.length === 1 ? '' : 's'}`;
    sectionHtml.push(`
      <section id="${sectionId}" class="model-section ${isCollapsedByDefault ? 'is-collapsed' : ''}">
        <button class="model-section-toggle" type="button" data-section-id="${sectionId}">
          <span class="model-section-title-wrap">
            <span class="model-section-chevron">${isCollapsedByDefault ? '▸' : '▾'}</span>
            <span class="model-section-title">${escapeHtml(recipeDisplayName(recipe))}</span>
          </span>
          <span class="model-section-count">${escapeHtml(modelCount)}</span>
        </button>
        <div class="model-grid">
          ${cards}
        </div>
      </section>
    `);
  });

  sectionsHost.innerHTML = sectionHtml.join('');
  wireSectionToggles();
  wireModelCardCopyButtons();
}

async function fetchLatestVTag() {
  const response = await fetch(TAGS_URL);
  if (!response.ok) {
    throw new Error(`Tag lookup failed: HTTP ${response.status}`);
  }
  const tags = await response.json();
  if (!Array.isArray(tags)) {
    throw new Error('Tag response is invalid');
  }
  const match = tags.find((tag) => typeof tag.name === 'string' && /^v/.test(tag.name));
  if (!match) {
    throw new Error('No v* tag found');
  }
  return match.name;
}

async function loadModelsData() {
  const loading = document.getElementById('models-loading');
  const error = document.getElementById('models-error');
  const sectionsHost = document.getElementById('supported-model-sections');
  const empty = document.getElementById('models-empty');
  const context = document.getElementById('models-context');
  const hotPicksSection = document.getElementById('hot-picks-section');
  const hotPicksGrid = document.getElementById('hot-picks-grid');

  loading.classList.remove('hidden');
  error.classList.add('hidden');
  sectionsHost.classList.add('hidden');
  empty.classList.add('hidden');
  context.classList.add('hidden');
  hotPicksSection.classList.add('hidden');
  hotPicksGrid.innerHTML = '';

  const tag = await fetchLatestVTag();
  const sourceUrl = `${RAW_BASE}/${tag}/src/cpp/resources/server_models.json`;
  const response = await fetch(sourceUrl);
  if (!response.ok) {
    throw new Error(`Model JSON fetch failed: HTTP ${response.status}`);
  }
  const data = await response.json();
  if (!data || typeof data !== 'object' || Array.isArray(data)) {
    throw new Error('Model JSON payload is invalid');
  }

  state.tag = tag;
  state.sourceUrl = sourceUrl;
  state.models = Object.entries(data).map(([name, details]) => ({ name, details }));

  const releaseTagLink = document.getElementById('release-tag-link');
  const releaseTagText = document.getElementById('release-tag-text');
  releaseTagText.textContent = `Lemonade ${tag}`;
  releaseTagLink.href = sourceUrl;
  releaseTagLink.classList.remove('hidden');

  renderRecipeButtons();
  renderLabelButtons();
  renderModels();
}

function wireControls() {
  const searchInput = document.getElementById('model-search-input');
  const retryButton = document.getElementById('retry-load-btn');
  const modelsContext = document.getElementById('models-context');
  const hotPicksViewAllButton = document.getElementById('hot-picks-view-all-btn');
  const sidebarToggle = document.getElementById('models-sidebar-toggle');
  const sidebar = document.getElementById('models-sidebar');
  const sidebarBackdrop = document.getElementById('models-sidebar-backdrop');
  const pageBody = document.getElementById('models-page-body');

  function setSidebarOpen(isOpen) {
    if (!sidebarToggle || !sidebar || !sidebarBackdrop || !pageBody) return;
    pageBody.classList.toggle('models-sidebar-open', isOpen);
    sidebarToggle.setAttribute('aria-expanded', isOpen ? 'true' : 'false');
    sidebarBackdrop.setAttribute('aria-hidden', isOpen ? 'false' : 'true');
  }

  searchInput.addEventListener('input', () => {
    state.search = searchInput.value.trim().toLowerCase();
    if (state.label === 'hot') {
      state.label = 'all';
      renderLabelButtons();
    }
    renderModels();
  });
  retryButton.addEventListener('click', () => {
    initializeModelsPage();
  });

  if (hotPicksViewAllButton) {
    hotPicksViewAllButton.addEventListener('click', () => {
      state.label = 'hot';
      renderLabelButtons();
      renderModels();
      const sections = document.getElementById('supported-model-sections');
      if (sections) {
        sections.scrollIntoView({ behavior: 'smooth', block: 'start' });
      }
    });
  }

  if (modelsContext) {
    modelsContext.addEventListener('click', (event) => {
      const clearButton = event.target.closest('#models-clear-filters-btn');
      if (!clearButton) return;
      state.recipe = 'all';
      state.label = 'all';
      state.search = '';
      searchInput.value = '';
      renderRecipeButtons();
      renderLabelButtons();
      renderModels();
    });
  }

  if (sidebarToggle && sidebar && sidebarBackdrop && pageBody) {
    sidebarToggle.addEventListener('click', () => {
      const willOpen = !pageBody.classList.contains('models-sidebar-open');
      setSidebarOpen(willOpen);
    });

    sidebarBackdrop.addEventListener('click', () => {
      setSidebarOpen(false);
    });

    document.addEventListener('keydown', (event) => {
      if (event.key === 'Escape') {
        setSidebarOpen(false);
      }
    });

    sidebar.addEventListener('click', (event) => {
      const targetButton = event.target.closest('.backend-filter-btn, .label-filter-btn');
      if (targetButton && window.matchMedia('(max-width: 1020px)').matches) {
        setSidebarOpen(false);
      }
    });

    window.addEventListener('resize', () => {
      if (window.innerWidth > 1020) {
        setSidebarOpen(false);
      }
    });
  }
}

async function initializeModelsPage() {
  const loading = document.getElementById('models-loading');
  const error = document.getElementById('models-error');
  try {
    if (!window.__modelsPageControlsBound) {
      wireControls();
      window.__modelsPageControlsBound = true;
    }
    await loadModelsData();
  } catch (err) {
    console.error('Failed to initialize models page:', err);
    loading.classList.add('hidden');
    error.classList.remove('hidden');
  }
}
