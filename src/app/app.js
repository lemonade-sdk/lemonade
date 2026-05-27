// app.js — interactivity for the lemonade UI redesign prototype.
//
// Wired to the lemond HTTP API via api.js (window.LemonadeAPI).
// Vanilla JS, no framework, no build step.

(() => {
  'use strict';

  const api = window.LemonadeAPI;

  // Tiny query helpers.
  const $ = (sel, root) => (root || document).querySelector(sel);
  const $$ = (sel, root) => [...(root || document).querySelectorAll(sel)];

  // ===================================================================
  // View switching
  // ===================================================================
  const navButtons = $$('[data-view-target]');
  const views = $$('.view');

  function activateView(name) {
    navButtons.forEach(b => b.classList.toggle('is-active', b.dataset.viewTarget === name));
    views.forEach(v => v.classList.toggle('is-active', v.dataset.view === name));
    if (name === 'models' && api.isConnected) refreshModelsView();
    if (name === 'backends' && api.isConnected) refreshBackendsView();
  }

  navButtons.forEach(btn => {
    btn.addEventListener('click', () => activateView(btn.dataset.viewTarget));
  });

  // ===================================================================
  // Conversation rail expand / collapse
  // ===================================================================
  $$('[data-rail-toggle]').forEach(toggle => {
    toggle.addEventListener('click', e => {
      const chat = e.currentTarget.closest('.chat');
      if (chat) chat.classList.toggle('rail-expanded');
    });
  });

  // ===================================================================
  // Status pill expand / collapse
  // ===================================================================
  $$('[data-status-toggle]').forEach(pill => {
    pill.addEventListener('click', e => {
      const view = e.currentTarget.closest('.view');
      if (!view) return;
      const strip = view.querySelector('.status-strip');
      if (!strip) return;
      strip.classList.add('is-open');
      e.currentTarget.classList.add('is-hidden');
    });
  });

  $$('[data-status-close]').forEach(close => {
    close.addEventListener('click', e => {
      const view = e.currentTarget.closest('.view');
      if (!view) return;
      const strip = view.querySelector('.status-strip');
      const pill = view.querySelector('.status-pill');
      if (strip) strip.classList.remove('is-open');
      if (pill) pill.classList.remove('is-hidden');
    });
  });

  // ===================================================================
  // Capability chips (visual feedback only)
  // ===================================================================
  $$('.chip').forEach(chip => {
    chip.addEventListener('click', () => {
      chip.classList.add('is-pressed');
      setTimeout(() => chip.classList.remove('is-pressed'), 700);
    });
  });

  // ===================================================================
  // Filter chips — toggle on/off + real filtering
  // ===================================================================
  $$('.filter-chip').forEach(chip => {
    chip.addEventListener('click', () => {
      chip.classList.toggle('is-on');
      applyModelFilters();
    });
  });

  function applyModelFilters() {
    const activeFilters = $$('.filter-chip.is-on').map(c => c.textContent.trim().toLowerCase());
    if (activeFilters.length === 0) {
      $$('.row[data-row]').forEach(r => (r.style.display = ''));
      return;
    }
    $$('.row[data-row]').forEach(row => {
      const badges = $$('.cap-badge, .row__device', row).map(el => el.textContent.trim().toLowerCase());
      const name = (row.dataset.name || '').toLowerCase();
      const family = (row.dataset.family || '').toLowerCase();
      const size = parseFloat(row.dataset.sizeGb || '0');
      const match = activeFilters.some(f => {
        if (f === '< 5 gb') return size > 0 && size < 5;
        if (f === '5–20 gb' || f === '5-20 gb') return size >= 5 && size <= 20;
        if (f === '20+ gb') return size > 20;
        return badges.includes(f) || family.includes(f) || name.includes(f);
      });
      row.style.display = match ? '' : 'none';
    });
  }

  // ===================================================================
  // Slide-over: Model detail panel
  // ===================================================================
  const scrim = $('#scrim');
  const slideover = $('#model-slideover');
  const slideoverTitle = slideover?.querySelector('[data-slideover-title]');
  const slideoverSub = slideover?.querySelector('[data-slideover-sub]');
  const slideoverDesc = slideover?.querySelector('[data-slideover-desc]');

  function openSlideover(model) {
    if (!slideover || !scrim) return;
    if (slideoverTitle) slideoverTitle.textContent = model.name || model.id || '';
    if (slideoverSub) slideoverSub.textContent = model.sub || `${model.family || ''} · ${model.params || ''} · ${model.quant || ''}`;
    if (slideoverDesc) slideoverDesc.textContent = model.description || 'A capable, locally-served model.';
    slideover.classList.add('is-open');
    scrim.classList.add('is-open');
  }

  function closeSlideover() {
    if (!slideover || !scrim) return;
    slideover.classList.remove('is-open');
    scrim.classList.remove('is-open');
  }

  // Event delegation for dynamically rendered rows
  document.addEventListener('click', e => {
    const row = e.target.closest('[data-row]');
    if (!row) return;
    if (e.target.closest('button.row__action, [data-action-load], [data-action-unload], [data-action-pull]')) return;
    const data = {
      name: row.dataset.name || 'Untitled model',
      family: row.dataset.family || '—',
      params: row.dataset.params || '—',
      quant: row.dataset.quant || '—',
      description: row.dataset.description || 'A capable, locally-served model.',
    };
    openSlideover(data);
  });

  $$('[data-slideover-close]').forEach(b => b.addEventListener('click', closeSlideover));
  scrim?.addEventListener('click', () => { closeSlideover(); closeRecipeSlideover(); });
  document.addEventListener('keydown', e => {
    if (e.key === 'Escape') {
      closeSlideover();
      if (recipeSlideover?.classList.contains('is-open')) closeRecipeSlideover();
    }
  });

  // ===================================================================
  // Connect / Discover sub-tabs
  // ===================================================================
  $$('[data-subtab]').forEach(tab => {
    tab.addEventListener('click', () => {
      const target = tab.dataset.subtab;
      const root = tab.closest('.connect');
      if (!root) return;
      root.querySelectorAll('[data-subtab]').forEach(t => t.classList.toggle('is-active', t === tab));
      root.querySelectorAll('[data-subview]').forEach(v => v.classList.toggle('is-active', v.dataset.subview === target));
    });
  });

  $$('[data-empty-go]').forEach(link => {
    link.addEventListener('click', e => { e.preventDefault(); activateView(link.dataset.emptyGo); });
  });

  // ===================================================================
  // Backend Manager: show technical details toggle
  // ===================================================================
  const techToggle = $('#tech-details-toggle');
  techToggle?.addEventListener('change', e => {
    const matrix = $('.matrix');
    if (matrix) matrix.classList.toggle('show-tech', e.target.checked);
  });

  // ===================================================================
  // Composer: textarea auto-grow + send
  // ===================================================================
  $$('.composer__input').forEach(ta => {
    ta.addEventListener('input', () => {
      ta.style.height = 'auto';
      ta.style.height = Math.min(ta.scrollHeight, 200) + 'px';
    });
    ta.addEventListener('keydown', e => {
      if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        handleSendMessage();
      }
    });
  });

  $$('.composer__send').forEach(btn => {
    btn.addEventListener('click', () => handleSendMessage());
  });

  // ===================================================================
  // Toast
  // ===================================================================
  function toast(msg) {
    let el = $('#toast');
    if (!el) {
      el = document.createElement('div');
      el.id = 'toast';
      el.className = 'toast';
      document.body.appendChild(el);
    }
    el.textContent = msg;
    requestAnimationFrame(() => el.classList.add('is-visible'));
    clearTimeout(el._t);
    el._t = setTimeout(() => el.classList.remove('is-visible'), 2200);
  }

  // ===================================================================
  // Utility
  // ===================================================================
  function escapeHTML(str) {
    const div = document.createElement('div');
    div.textContent = str;
    return div.innerHTML;
  }

  // ===================================================================
  // Presets (100% client-side — invariant #11)
  // ===================================================================
  const STARTERS = [
    { id: 's-balanced', name: 'Balanced', description: 'Sensible defaults. Good first pick for everyday chat.', applies_to: ['chat'], options: { ctx_size: 4096, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 0.70, top_p: 0.90, top_k: 40, repeat_penalty: 1.05 }, starter: true },
    { id: 's-quality', name: 'Quality', description: 'Larger context, slightly looser sampling for richer long-form answers.', applies_to: ['chat'], options: { ctx_size: 8192, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 0.70, top_p: 0.95, top_k: 40, repeat_penalty: 1.10 }, starter: true },
    { id: 's-fast', name: 'Fast', description: 'Small context, tight sampling. Snappy responses for quick interactions.', applies_to: ['chat'], options: { ctx_size: 2048, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 0.60, top_p: 0.80, top_k: 40, repeat_penalty: 1.05 }, starter: true },
    { id: 's-creative', name: 'Creative', description: 'Higher temperature for brainstorming, dialog, and divergent thinking.', applies_to: ['chat'], options: { ctx_size: 8192, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 0.95, top_p: 0.95, top_k: 60, repeat_penalty: 1.00 }, starter: true },
    { id: 's-long-context', name: 'Long Context', description: 'For documents, codebases, and long conversation threads.', applies_to: ['chat'], options: { ctx_size: 32768, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 0.70, top_p: 0.90, top_k: 40, repeat_penalty: 1.05 }, starter: true },
    { id: 's-code', name: 'Code', description: 'Low temperature, tight sampling for code generation and refactoring.', applies_to: ['chat'], options: { ctx_size: 8192, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 0.20, top_p: 0.95, top_k: 40, repeat_penalty: 1.05 }, starter: true },
    { id: 's-sharp', name: 'Sharp', description: 'More steps and tighter guidance for crisp, deliberate image generation.', applies_to: ['image'], options: { steps: 30, cfg_scale: 8.0 }, sampling: {}, starter: true },
    { id: 's-quick', name: 'Quick', description: 'Fewer steps, looser guidance — fast drafts and iteration.', applies_to: ['image'], options: { steps: 15, cfg_scale: 7.0 }, sampling: {}, starter: true },
  ];

  const YOURS = [
    { id: 'u-long-code', name: 'Long Code', description: 'Custom: big context + code-style sampling for monorepo work.', applies_to: ['chat'], options: { ctx_size: 16384, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 0.25, top_p: 0.95, top_k: 40, repeat_penalty: 1.04 }, starter: false },
    { id: 'u-brainstorm', name: 'Brainstorm', description: 'High-temp, wide top_p for ideation sessions and divergent thinking.', applies_to: ['chat'], options: { ctx_size: 4096, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 1.05, top_p: 0.98, top_k: 80, repeat_penalty: 1.00 }, starter: false },
  ];

  const appliedRecipes = {
    'Qwen3-26B': 's-quality',
    'Gemma-3-12B-it': 's-balanced',
    'Llama-3.2-3B-Instruct': 's-fast',
    'Mistral-Small-3.1-24B-Instruct': 'u-long-code',
    'stable-diffusion-3.5-medium': 's-sharp',
  };

  let activeRecipe = null;
  const recipeSlideover = $('#recipe-slideover');

  const lookupRecipe = (id) => STARTERS.find(r => r.id === id) || YOURS.find(r => r.id === id) || null;
  const allRecipes = () => [...STARTERS, ...YOURS];

  // ===================================================================
  // Label system — derives from API data, falls back to inference
  // ===================================================================

  const LABEL_MAP = {
    'reasoning': { cls: 'chat', text: 'Chat' },
    'coding':    { cls: 'code', text: 'Code' },
    'vision':    { cls: 'vision', text: 'Vision' },
    'tool-calling': { cls: 'chat', text: 'Tools' },
  };

  function labelsFor(modelNameOrObj) {
    const model = typeof modelNameOrObj === 'string'
      ? (api.allModels.find(m => m.id === modelNameOrObj) || null)
      : modelNameOrObj;

    const caps = [];
    if (model) {
      if (model.labels && model.labels.length > 0) {
        for (const label of model.labels) {
          const mapped = LABEL_MAP[label];
          if (mapped) caps.push(mapped.cls);
          else caps.push(label);
        }
      }
      const recipe = (model.recipe || '').toLowerCase();
      const name = (model.id || '').toLowerCase();
      if (recipe.includes('whisper') || (recipe === 'flm' && (name.includes('whisper') || name.includes('parakeet')))) caps.push('audio');
      if (recipe === 'kokoro') caps.push('tts');
      if (recipe === 'sd-cpp') caps.push('image');
      if (name.includes('embed')) caps.push('embed');
      if (name.includes('rerank')) caps.push('rerank');
    }
    const unique = [...new Set(caps)];
    return unique.length > 0 ? unique : ['chat'];
  }

  function presetLabelsFor(modelName) {
    const caps = labelsFor(modelName);
    const capMap = { chat: 'chat', code: 'chat', vision: 'chat', audio: 'transcription',
                     tts: 'tts', image: 'image', embed: 'embedding', rerank: 'reranking' };
    return [...new Set(caps.map(c => capMap[c] || c))];
  }

  const isCompatible = (preset, modelName) =>
    preset.applies_to.some(cap => presetLabelsFor(modelName).includes(cap));

  const primaryCap = (preset) => preset.applies_to[0] || 'chat';

  const paramsPreview = (r) => {
    if (primaryCap(r) === 'image') {
      const s = r.options.steps ?? '—';
      const c = r.options.cfg_scale != null ? r.options.cfg_scale.toFixed(1) : '—';
      return `steps ${s} · cfg ${c}`;
    }
    const t = r.sampling?.temperature != null ? r.sampling.temperature.toFixed(2) : '—';
    const ctx = r.options.ctx_size ?? '—';
    return `temp ${t} · ctx ${ctx}`;
  };

  // ===================================================================
  // Badge HTML helper
  // ===================================================================
  const BADGE_TEXT = {
    chat: 'Chat', code: 'Code', vision: 'Vision', audio: 'Audio',
    tts: 'TTS', image: 'Image', embed: 'Embed', rerank: 'Rerank',
  };

  function badgeHTML(capClass) {
    const text = BADGE_TEXT[capClass] || capClass;
    return `<span class="cap-badge cap-badge--${capClass}">${text}</span>`;
  }

  // ===================================================================
  // Preset card rendering
  // ===================================================================
  function recipeCardHTML(r) {
    const params = paramsPreview(r);
    return `
      <article class="recipe-card" data-recipe-id="${r.id}" tabindex="0" role="button"
               aria-label="Preset: ${r.name}">
        ${r.starter ? '<span class="starter-badge">Starter</span>' : ''}
        <div class="recipe-card__head">
          <span class="phase-glyph" aria-hidden="true">
            <svg width="14" height="14" viewBox="0 0 12 12" fill="none">
              <circle cx="6" cy="6" r="5" stroke="currentColor" stroke-width="1.3" />
              <path d="M6 1 A5 5 0 0 1 6 11 Z" fill="currentColor" />
            </svg>
          </span>
          <span class="recipe-card__name">${r.name}</span>
        </div>
        <p class="recipe-card__desc">${r.description}</p>
        <div class="cap-chip-list cap-chip-list--card" title="Applies to capabilities">
          ${r.applies_to.map(c => `<span class="cap-chip cap-chip--${c}"><span class="cap-chip__dot" aria-hidden="true"></span>${c}</span>`).join('')}
        </div>
        <div class="recipe-card__params" aria-hidden="true">
          <span class="recipe-card__param-key">params</span>
          <span class="recipe-card__param-val">${params}</span>
        </div>
        <div class="recipe-card__actions">
          ${r.starter
            ? '<button class="recipe-card__action recipe-card__action--primary" data-card-action="clone">Clone</button>'
            : '<button class="recipe-card__action" data-card-action="apply">Apply</button><button class="recipe-card__action" data-card-action="export">Export</button>'}
        </div>
      </article>`;
  }

  function renderRecipeGrid() {
    const starterEl = $('[data-recipe-grid="starters"]');
    const yoursEl = $('[data-recipe-grid="yours"]');
    if (starterEl) starterEl.innerHTML = STARTERS.map(recipeCardHTML).join('');
    if (yoursEl)   yoursEl.innerHTML = YOURS.map(recipeCardHTML).join('');
    const count = $('[data-recipes-count]');
    if (count) count.textContent = `${STARTERS.length} starters · ${YOURS.length} yours`;
    const yoursCount = $('[data-yours-count]');
    if (yoursCount) yoursCount.textContent = String(YOURS.length);
    const empty = $('[data-empty="yours"]');
    if (empty) empty.hidden = YOURS.length > 0;
  }

  function renderAppliedList() {
    const root = $('[data-applied-list]');
    if (!root) return;
    const modelNames = api.allModels.length > 0
      ? api.allModels.slice(0, 8).map(m => m.id)
      : Object.keys(appliedRecipes);
    root.innerHTML = modelNames.map(name => {
      const initial = name.charAt(0);
      const rid = appliedRecipes[name];
      const recipe = rid ? lookupRecipe(rid) : null;
      const recipeCell = recipe
        ? `<div class="applied-row__recipe">
             <span class="phase-glyph" aria-hidden="true"><svg width="12" height="12" viewBox="0 0 12 12" fill="none"><circle cx="6" cy="6" r="5" stroke="currentColor" stroke-width="1.3" /><path d="M6 1 A5 5 0 0 1 6 11 Z" fill="currentColor" /></svg></span>
             <span class="applied-row__recipe-name">${recipe.name}</span>
             <span style="font-size:11px;color:var(--text-tertiary);font-family:var(--font-mono);">· ${recipe.starter ? 'starter' : 'yours'}</span>
           </div>`
        : `<div class="applied-row__recipe applied-row__recipe--none">no preset — defaults</div>`;
      const actions = recipe
        ? `<button class="btn btn--tiny btn--ghost" data-applied-edit="${rid}">Edit</button>
           <button class="btn btn--tiny btn--ghost" data-applied-detach="${name}">Detach</button>`
        : `<button class="btn btn--tiny btn--ghost" data-applied-pick="${name}">Apply…</button>`;
      return `<div class="applied-row" data-applied-row="${name}">
          <div class="applied-row__model"><span class="applied-row__model-icon">${initial}</span><span class="applied-row__model-name">${name}</span></div>
          ${recipeCell}
          <div class="applied-row__actions">${actions}</div>
        </div>`;
    }).join('');
  }

  function renderRowRecipeChips() {
    $$('.row[data-name]').forEach(row => {
      row.querySelectorAll('.row__recipe-chip').forEach(n => n.remove());
      const name = row.getAttribute('data-name');
      const rid = appliedRecipes[name];
      if (!rid) return;
      const recipe = lookupRecipe(rid);
      if (!recipe) return;
      const right = row.querySelector('.row__right');
      if (!right) return;
      const chip = document.createElement('span');
      chip.className = 'row__recipe-chip';
      chip.title = `Preset: ${recipe.name}`;
      chip.innerHTML = `<span class="phase-glyph" aria-hidden="true"><svg width="10" height="10" viewBox="0 0 12 12" fill="none"><circle cx="6" cy="6" r="5" stroke="currentColor" stroke-width="1.4" /><path d="M6 1 A5 5 0 0 1 6 11 Z" fill="currentColor" /></svg></span>${recipe.name}`;
      right.insertBefore(chip, right.firstChild);
    });
  }

  // ===================================================================
  // Preset slide-over
  // ===================================================================
  function openRecipeSlideover(recipe) {
    if (!recipeSlideover || !recipe) return;
    activeRecipe = recipe;
    $('[data-recipe-name]', recipeSlideover).textContent = recipe.name;
    $('[data-recipe-desc]', recipeSlideover).textContent = recipe.description;
    $('[data-recipe-starter-badge]', recipeSlideover).hidden = !recipe.starter;

    const enginesEl = $('[data-recipe-engines]', recipeSlideover);
    const KNOWN = ['chat','vision','code','embedding','reranking','image','edit','transcription','tts'];
    enginesEl.innerHTML = KNOWN.map(c => {
      const on = recipe.applies_to.includes(c);
      return `<button class="cap-chip ${on ? 'is-on' : 'is-off'} cap-chip--${c}" type="button" ${recipe.starter ? 'disabled' : ''} data-cap-chip="${c}"><span class="cap-chip__dot" aria-hidden="true"></span>${c}</button>`;
    }).join('');

    const cap = primaryCap(recipe);
    recipeSlideover.querySelectorAll('[data-preset-fields]').forEach(el => {
      el.hidden = el.getAttribute('data-preset-fields') !== cap;
    });

    if (cap === 'image') {
      const stepsEl = $('[data-recipe-steps]', recipeSlideover);
      const stepsValEl = $('[data-recipe-steps-val]', recipeSlideover);
      const cfgEl = $('[data-recipe-cfg]', recipeSlideover);
      const cfgValEl = $('[data-recipe-cfg-val]', recipeSlideover);
      if (stepsEl) { stepsEl.value = recipe.options.steps ?? 30; stepsEl.disabled = recipe.starter; }
      if (stepsValEl) stepsValEl.textContent = String(recipe.options.steps ?? 30);
      if (cfgEl) { cfgEl.value = recipe.options.cfg_scale ?? 7.0; cfgEl.disabled = recipe.starter; }
      if (cfgValEl) cfgValEl.textContent = (recipe.options.cfg_scale ?? 7.0).toFixed(1);
    } else {
      $('[data-recipe-ctx]', recipeSlideover).value = recipe.options.ctx_size;
      $('[data-recipe-ctx-val]', recipeSlideover).textContent = recipe.options.ctx_size;
      const backendSel = $('[data-recipe-backend]', recipeSlideover);
      [...backendSel.options].forEach(opt => { opt.selected = opt.textContent === recipe.options.backend; });
      $('[data-recipe-temp]', recipeSlideover).value = recipe.sampling.temperature;
      $('[data-recipe-temp-val]', recipeSlideover).textContent = recipe.sampling.temperature.toFixed(2);
      $('[data-recipe-top-p]', recipeSlideover).value = recipe.sampling.top_p;
      $('[data-recipe-top-p-val]', recipeSlideover).textContent = recipe.sampling.top_p.toFixed(2);
      $('[data-recipe-top-k]', recipeSlideover).value = recipe.sampling.top_k;
      $('[data-recipe-rp]', recipeSlideover).value = recipe.sampling.repeat_penalty;
      $('[data-recipe-rp-val]', recipeSlideover).textContent = recipe.sampling.repeat_penalty.toFixed(2);
      const readOnly = recipe.starter;
      ['[data-recipe-ctx]','[data-recipe-backend]','[data-recipe-temp]',
       '[data-recipe-top-p]','[data-recipe-top-k]','[data-recipe-rp]'].forEach(s => {
        const el = $(s, recipeSlideover); if (el) el.disabled = readOnly;
      });
      const discl = recipeSlideover.querySelector('.disclosure');
      if (discl) discl.style.display = readOnly ? 'none' : '';
    }

    $('[data-recipe-clone]', recipeSlideover).hidden = !recipe.starter;
    $('[data-recipe-save]', recipeSlideover).hidden = recipe.starter;
    $('[data-recipe-delete]', recipeSlideover).hidden = recipe.starter;

    const tgt = $('[data-recipe-apply-target]', recipeSlideover);
    const names = api.allModels.length > 0 ? api.allModels.map(m => m.id) : Object.keys(appliedRecipes);
    tgt.innerHTML = '<option value="">— pick a model —</option>' +
      names.map(name => {
        const compat = isCompatible(recipe, name);
        return `<option value="${name}" ${compat ? '' : 'disabled'}>${name}${compat ? '' : ' — incompatible'}</option>`;
      }).join('');

    $('#scrim').classList.add('is-open');
    recipeSlideover.classList.add('is-open');
    recipeSlideover.setAttribute('aria-hidden', 'false');
  }

  function closeRecipeSlideover() {
    if (!recipeSlideover) return;
    recipeSlideover.classList.remove('is-open');
    recipeSlideover.setAttribute('aria-hidden', 'true');
    if (!$('#model-slideover')?.classList.contains('is-open')) {
      $('#scrim').classList.remove('is-open');
    }
    activeRecipe = null;
  }

  // Slider mirroring
  function wireSliderMirror(sliderSel, valueSel, decimals) {
    const slider = $(sliderSel, recipeSlideover);
    const valueEl = $(valueSel, recipeSlideover);
    if (!slider || !valueEl) return;
    slider.addEventListener('input', () => {
      const n = Number(slider.value);
      valueEl.textContent = decimals == null ? String(n) : n.toFixed(decimals);
    });
  }
  wireSliderMirror('[data-recipe-ctx]', '[data-recipe-ctx-val]', null);
  wireSliderMirror('[data-recipe-temp]', '[data-recipe-temp-val]', 2);
  wireSliderMirror('[data-recipe-top-p]', '[data-recipe-top-p-val]', 2);
  wireSliderMirror('[data-recipe-rp]', '[data-recipe-rp-val]', 2);
  wireSliderMirror('[data-recipe-steps]', '[data-recipe-steps-val]', null);
  wireSliderMirror('[data-recipe-cfg]', '[data-recipe-cfg-val]', 1);

  // Apply / detach
  function applyRecipeToModel(rid, modelName) {
    const recipe = lookupRecipe(rid);
    if (!recipe) return;
    if (!isCompatible(recipe, modelName)) {
      toast(`${recipe.name} doesn't apply to ${modelName}`);
      return;
    }
    appliedRecipes[modelName] = rid;
    renderAppliedList();
    renderRowRecipeChips();
    updateChatPill();
    toast(`Applied "${recipe.name}" to ${modelName}`);
  }

  function detachRecipe(modelName) {
    delete appliedRecipes[modelName];
    renderAppliedList();
    renderRowRecipeChips();
    updateChatPill();
    toast(`Detached preset from ${modelName}`);
  }

  // Card + applied-list click delegation
  document.addEventListener('click', e => {
    const action = e.target.closest('[data-card-action]');
    if (action) {
      e.stopPropagation();
      const card = action.closest('[data-recipe-id]');
      const rid = card?.getAttribute('data-recipe-id');
      const recipe = lookupRecipe(rid);
      const act = action.getAttribute('data-card-action');
      if (act === 'clone') toast(`Cloned "${recipe?.name}"`);
      else if (act === 'apply' && recipe) openRecipeSlideover(recipe);
      else if (act === 'export') toast(`Exported "${recipe?.name}" as JSON (mock)`);
      return;
    }
    const card = e.target.closest('.recipe-card[data-recipe-id]');
    if (card) { const r = lookupRecipe(card.getAttribute('data-recipe-id')); if (r) openRecipeSlideover(r); return; }
    const appliedEdit = e.target.closest('[data-applied-edit]');
    if (appliedEdit) { const r = lookupRecipe(appliedEdit.getAttribute('data-applied-edit')); if (r) openRecipeSlideover(r); return; }
    const appliedDetach = e.target.closest('[data-applied-detach]');
    if (appliedDetach) { detachRecipe(appliedDetach.getAttribute('data-applied-detach')); return; }
    const appliedPick = e.target.closest('[data-applied-pick]');
    if (appliedPick) {
      const mn = appliedPick.getAttribute('data-applied-pick');
      const c = allRecipes().find(r => isCompatible(r, mn));
      if (c) applyRecipeToModel(c.id, mn);
      return;
    }
  });

  $('[data-recipe-apply]')?.addEventListener('click', () => {
    const sel = $('[data-recipe-apply-target]');
    if (sel.value && activeRecipe) applyRecipeToModel(activeRecipe.id, sel.value);
  });
  $('[data-recipe-clone]')?.addEventListener('click', () => { if (activeRecipe) toast(`Cloned "${activeRecipe.name}"`); });
  $('[data-recipe-save]')?.addEventListener('click', () => { if (activeRecipe) toast(`Saved changes to "${activeRecipe.name}"`); });
  $('[data-recipe-delete]')?.addEventListener('click', () => { if (activeRecipe) { toast(`Deleted "${activeRecipe.name}"`); closeRecipeSlideover(); } });
  $('[data-recipe-export]')?.addEventListener('click', () => { if (activeRecipe) toast(`Exported "${activeRecipe.name}" as JSON (mock)`); });

  document.addEventListener('click', e => {
    if (e.target.closest('#recipe-slideover [data-slideover-close]')) closeRecipeSlideover();
  });

  // Drag-and-drop import
  const recipesView = $('[data-recipes-drop]');
  if (recipesView) {
    let counter = 0;
    recipesView.addEventListener('dragenter', e => { e.preventDefault(); counter++; recipesView.classList.add('is-dropping'); });
    recipesView.addEventListener('dragover', e => e.preventDefault());
    recipesView.addEventListener('dragleave', () => { counter = Math.max(0, counter - 1); if (counter === 0) recipesView.classList.remove('is-dropping'); });
    recipesView.addEventListener('drop', e => { e.preventDefault(); counter = 0; recipesView.classList.remove('is-dropping'); toast(`Imported "${e.dataTransfer?.files?.[0]?.name || 'preset.json'}" (mock)`); });
  }

  // Import dropdown
  $$('[data-dropdown]').forEach(dd => {
    const trigger = dd.querySelector('[data-dropdown-trigger]');
    const menu = dd.querySelector('.dropdown__menu');
    if (!trigger || !menu) return;
    trigger.addEventListener('click', e => { e.stopPropagation(); menu.hidden = !menu.hidden; trigger.setAttribute('aria-expanded', String(!menu.hidden)); });
    menu.addEventListener('click', e => { const item = e.target.closest('.dropdown__item'); if (!item) return; menu.hidden = true; trigger.setAttribute('aria-expanded', 'false'); toast(item.getAttribute('data-import-source') === 'file' ? 'Pick a preset JSON…' : 'Pasted preset from clipboard'); });
  });
  document.addEventListener('click', () => {
    $$('[data-dropdown]').forEach(dd => { const m = dd.querySelector('.dropdown__menu'); if (m && !m.hidden) { m.hidden = true; dd.querySelector('[data-dropdown-trigger]')?.setAttribute('aria-expanded', 'false'); } });
  });
  $$('[data-recipe-new]').forEach(btn => btn.addEventListener('click', () => toast('New preset — form would open here')));

  // ===================================================================
  // Chat composer recipe pill + popover
  // ===================================================================
  const chatPill = $('[data-chat-recipe-pill]');
  const popover = $('[data-recipe-popover]');
  let currentChatModel = 'Qwen3-26B';

  function updateChatPill() {
    if (!chatPill) return;
    const rid = appliedRecipes[currentChatModel];
    const recipe = rid ? lookupRecipe(rid) : null;
    $('[data-chat-pill-model]', chatPill).textContent = currentChatModel;
    $('[data-chat-pill-recipe]', chatPill).textContent = recipe ? `${recipe.name} preset` : 'No preset';
  }

  function renderPopover() {
    if (!popover) return;
    const rid = appliedRecipes[currentChatModel];
    const recipe = rid ? lookupRecipe(rid) : null;
    $('[data-popover-current]', popover).innerHTML = recipe
      ? `<span class="phase-glyph" aria-hidden="true"><svg width="12" height="12" viewBox="0 0 12 12" fill="none"><circle cx="6" cy="6" r="5" stroke="currentColor" stroke-width="1.3"/><path d="M6 1 A5 5 0 0 1 6 11 Z" fill="currentColor"/></svg></span><span><strong>${recipe.name}</strong> · <span style="font-family:var(--font-mono);font-size:11px;color:var(--text-tertiary)">${paramsPreview(recipe)}</span></span>`
      : 'No preset applied. Using defaults.';
    $('[data-popover-list]', popover).innerHTML = allRecipes().map(r => {
      const compat = isCompatible(r, currentChatModel);
      const active = r.id === rid ? 'style="background:var(--surface-3);color:var(--text-primary);"' : '';
      return `<li class="recipe-popover__item" role="option" ${compat ? '' : 'aria-disabled="true"'} ${active} data-popover-pick="${r.id}">
          <span class="phase-glyph" aria-hidden="true"><svg width="11" height="11" viewBox="0 0 12 12" fill="none"><circle cx="6" cy="6" r="5" stroke="currentColor" stroke-width="1.3"/><path d="M6 1 A5 5 0 0 1 6 11 Z" fill="currentColor"/></svg></span>
          <span class="recipe-popover__item-name">${r.name}${r.starter ? '' : ' <span style="font-size:10px;color:var(--text-tertiary)">· yours</span>'}</span>
          <span class="recipe-popover__item-engine">${r.applies_to.join(',')}</span>
        </li>`;
    }).join('');
  }

  function openPopover() { if (!popover) return; renderPopover(); popover.hidden = false; chatPill?.setAttribute('aria-expanded', 'true'); }
  function closePopover() { if (!popover) return; popover.hidden = true; chatPill?.setAttribute('aria-expanded', 'false'); }

  chatPill?.addEventListener('click', e => { e.stopPropagation(); if (popover?.hidden) openPopover(); else closePopover(); });
  popover?.addEventListener('click', e => {
    e.stopPropagation();
    const item = e.target.closest('[data-popover-pick]');
    if (item && item.getAttribute('aria-disabled') !== 'true') { applyRecipeToModel(item.getAttribute('data-popover-pick'), currentChatModel); closePopover(); return; }
    if (e.target.closest('[data-popover-fork]')) { toast('Per-conversation override (mock)'); closePopover(); }
  });
  document.addEventListener('click', e => { if (popover && !popover.hidden && !popover.contains(e.target) && e.target !== chatPill) closePopover(); });
  document.addEventListener('keydown', e => { if (e.key === 'Escape' && popover && !popover.hidden) closePopover(); });

  // Model slide-over recipe section
  document.addEventListener('click', e => {
    const row = e.target.closest('[data-row]');
    if (!row) return;
    const modelName = row.getAttribute('data-name');
    if (!modelName) return;
    setTimeout(() => {
      const section = $('[data-model-recipe]');
      if (!section) return;
      const rid = appliedRecipes[modelName];
      const recipe = rid ? lookupRecipe(rid) : null;
      const nameEl = $('[data-model-recipe-name]', section);
      const detachBtn = $('[data-model-recipe-detach]', section);
      nameEl.textContent = recipe ? `${recipe.name}${recipe.starter ? ' · starter' : ' · yours'}` : 'No preset';
      nameEl.classList.toggle('model-recipe__name--none', !recipe);
      detachBtn.hidden = !recipe;
      detachBtn.onclick = () => detachRecipe(modelName);
      const sel = $('[data-model-recipe-select]', section);
      sel.innerHTML = '<option value="">— pick a preset —</option>' +
        allRecipes().map(r => {
          const compat = isCompatible(r, modelName);
          return `<option value="${r.id}" ${r.id === rid ? 'selected' : ''} ${compat ? '' : 'disabled'}>${r.name}${compat ? '' : ' — incompatible'}</option>`;
        }).join('');
      sel.onchange = () => { if (sel.value) applyRecipeToModel(sel.value, modelName); else detachRecipe(modelName); };
      const hint = $('[data-model-recipe-hint]', section);
      if (hint) hint.textContent = `This model exposes [${labelsFor(modelName).join(', ')}]. Compatible presets only.`;
    }, 0);
  });

  // ===================================================================
  // Phase 1: Connection status UI
  // ===================================================================

  function updateStatusPill(status) {
    const dot = $('.status-pill__dot');
    if (dot) {
      dot.classList.remove('status-pill__dot--disconnected', 'status-pill__dot--connecting');
      if (status === 'disconnected') dot.classList.add('status-pill__dot--disconnected');
      else if (status === 'connecting') dot.classList.add('status-pill__dot--connecting');
    }
    const pill = $('.status-pill');
    if (!pill) return;
    const spans = pill.querySelectorAll('span');
    if (status === 'connected' && api.healthData) {
      const loaded = api.loadedModels;
      const primary = loaded.find(m => m.type === 'llm') || loaded[0];
      spans[1].textContent = 'Connected';
      spans[3].textContent = primary?.model_name || 'No model';
      spans[5].textContent = '';
    } else if (status === 'connecting') {
      spans[1].textContent = 'Connecting…';
      spans[3].textContent = '';
      spans[5].textContent = '';
    } else {
      spans[1].textContent = 'Not connected';
      spans[3].textContent = '';
      spans[5].textContent = '';
    }
  }

  function updateServerSettingsStatus(status) {
    const dot = $('[data-server-settings] .server-settings__dot');
    const text = $('[data-server-status-text]');
    if (!dot || !text) return;
    dot.classList.remove('server-settings__dot--on', 'server-settings__dot--off', 'server-settings__dot--connecting');
    if (status === 'connected') {
      dot.classList.add('server-settings__dot--on');
      const ver = api.healthData?.version || '';
      const count = api.loadedModels.length;
      text.textContent = `Connected${ver ? ` · v${ver}` : ''} · ${count} model${count !== 1 ? 's' : ''} loaded`;
    } else if (status === 'connecting') {
      dot.classList.add('server-settings__dot--connecting');
      text.textContent = 'Connecting…';
    } else {
      dot.classList.add('server-settings__dot--off');
      text.textContent = 'Not connected';
    }
  }

  function updateModelSelectorDot(status) {
    const dot = $('.model-selector__dot');
    if (!dot) return;
    if (status === 'connected') { dot.style.background = 'var(--success)'; dot.style.boxShadow = '0 0 0 3px var(--success-soft)'; }
    else if (status === 'connecting') { dot.style.background = 'var(--warn)'; dot.style.boxShadow = '0 0 0 3px var(--warn-soft)'; }
    else { dot.style.background = 'var(--danger)'; dot.style.boxShadow = '0 0 0 3px var(--danger-soft)'; }
  }

  api.onStatusChange(status => {
    updateStatusPill(status);
    updateServerSettingsStatus(status);
    updateModelSelectorDot(status);
  });

  // ===================================================================
  // Phase 2: Connect view — server settings
  // ===================================================================

  function initServerSettings() {
    const urlInput = $('[data-server-url]');
    const keyInput = $('[data-server-key]');
    const testBtn = $('[data-server-test]');
    const saveBtn = $('[data-server-save]');
    const resultEl = $('[data-server-result]');
    const resultInner = $('[data-server-result-inner]');

    if (urlInput) urlInput.value = api.baseUrl;
    if (keyInput) keyInput.value = api.apiKey;

    saveBtn?.addEventListener('click', () => {
      if (urlInput) api.baseUrl = urlInput.value.trim() || 'http://localhost:13305';
      if (keyInput) api.apiKey = keyInput.value.trim();
      toast('Server settings saved');
    });

    testBtn?.addEventListener('click', async () => {
      if (urlInput) api.baseUrl = urlInput.value.trim() || 'http://localhost:13305';
      if (keyInput) api.apiKey = keyInput.value.trim();
      resultEl.hidden = false;
      resultEl.className = 'server-settings__result';
      resultInner.textContent = 'Testing…';
      try {
        const health = await api.health();
        resultEl.classList.add('server-settings__result--ok');
        const loaded = health.all_models_loaded || [];
        resultInner.innerHTML = `<strong>✓ Connected</strong>
          <span>Version: ${health.version || 'unknown'}</span>
          <span>Models loaded: ${loaded.length}${loaded.length ? ' — ' + loaded.map(m => m.model_name).join(', ') : ''}</span>
          <code>${escapeHTML(api.baseUrl)}/api/v1/health</code>`;
        await api.models(true);
        refreshModelsView();
        refreshChatEmptyState();
        updateModelSelectorFromAPI();
      } catch (err) {
        resultEl.classList.add('server-settings__result--err');
        resultInner.innerHTML = `<strong>✗ Connection failed</strong><span>${escapeHTML(err.message)}</span><code>${escapeHTML(api.baseUrl)}</code>`;
      }
    });
  }

  // ===================================================================
  // Phase 3: Models view — dynamic rendering
  // ===================================================================

  function formatSize(sizeGb) {
    if (!sizeGb && sizeGb !== 0) return '—';
    if (sizeGb < 1) return `${Math.round(sizeGb * 1024)} MB`;
    return `${sizeGb.toFixed(1)} GB`;
  }

  function modelRowHTML(model, zone, loadedInfo) {
    const name = model.id || model.name || 'Unknown';
    const initial = name.charAt(0);
    const caps = labelsFor(model);
    const badges = caps.map(c => badgeHTML(c)).join('');
    const size = model.size ? formatSize(model.size) : '—';
    const sizeGb = model.size || 0;
    const recipe = model.recipe || '';
    let device = loadedInfo ? (loadedInfo.device || '').toUpperCase() : '';
    let actionBtn = '';

    if (zone === 'loaded') {
      actionBtn = `<button class="row__action row__action--ghost" data-action-unload="${escapeHTML(name)}">Unload</button>`;
    } else if (zone === 'installed') {
      actionBtn = `<button class="row__action" data-action-load="${escapeHTML(name)}">Load ▸</button>`;
    } else {
      actionBtn = `<button class="row__action" data-action-pull="${escapeHTML(name)}">Pull ▸</button>`;
    }

    return `<div class="row${zone === 'loaded' ? ' row--loaded' : ''}" data-row
           data-name="${escapeHTML(name)}" data-family="${escapeHTML(recipe)}"
           data-size-gb="${sizeGb}"
           data-description="${escapeHTML(model.description || '')}">
        <div class="row__main">
          <div class="row__icon">${initial}</div>
          <div class="row__text">
            <span class="row__name">${escapeHTML(name)}</span>
            <span class="row__sub">${escapeHTML(recipe)}${model.size ? ' · ' + size : ''}</span>
          </div>
        </div>
        <div class="row__right">
          ${badges}
          ${device ? `<span class="row__device">${device}</span>` : ''}
          <span class="row__size">${size}</span>
          ${actionBtn}
        </div>
      </div>`;
  }

  function renderModelsView(modelsData, healthData) {
    const body = $('[data-models-body]');
    if (!body) return;

    const allModels = modelsData?.data || [];
    const loadedList = healthData?.all_models_loaded || [];
    const loadedNames = new Set(loadedList.map(m => m.model_name));

    const loaded = [];
    const installed = [];
    const available = [];

    for (const model of allModels) {
      const name = model.id || model.name;
      if (loadedNames.has(name)) {
        loaded.push({ model, info: loadedList.find(m => m.model_name === name) });
      } else if (model.downloaded) {
        installed.push(model);
      } else {
        available.push(model);
      }
    }
    // Include loaded models not in the models list
    for (const lm of loadedList) {
      if (!allModels.find(m => (m.id || m.name) === lm.model_name)) {
        loaded.push({ model: { id: lm.model_name, recipe: lm.recipe, size: null }, info: lm });
      }
    }

    const sub = $('[data-models-subtitle]');
    if (sub) sub.textContent = `${loaded.length} loaded · ${installed.length} installed · ${available.length} available`;

    const emptyEl = body.querySelector('[data-empty="models"]');
    if (allModels.length === 0 && loadedList.length === 0) {
      if (emptyEl) emptyEl.hidden = false;
      body.querySelectorAll('.zone').forEach(z => z.remove());
      return;
    }
    if (emptyEl) emptyEl.hidden = true;

    // Remove all zones (static + dynamic)
    body.querySelectorAll('.zone').forEach(z => z.remove());

    const html = [];
    if (loaded.length > 0) {
      html.push(`<div class="zone"><div class="zone__head"><span class="zone__title">Loaded</span><span class="zone__count">${loaded.length}</span><span class="zone__rule"></span></div>
        ${loaded.map(({ model, info }) => modelRowHTML(model, 'loaded', info)).join('')}</div>`);
    }
    if (installed.length > 0) {
      html.push(`<div class="zone"><div class="zone__head"><span class="zone__title">Installed</span><span class="zone__count">${installed.length}</span><span class="zone__rule"></span></div>
        ${installed.map(m => modelRowHTML(m, 'installed')).join('')}</div>`);
    }
    if (available.length > 0) {
      html.push(`<div class="zone"><div class="zone__head"><span class="zone__title">Available</span><span class="zone__count">${available.length}</span><span class="zone__rule"></span></div>
        ${available.map(m => modelRowHTML(m, 'available')).join('')}</div>`);
    }

    body.insertAdjacentHTML('beforeend', html.join(''));
    renderRowRecipeChips();
  }

  async function refreshModelsView() {
    try {
      const result = await api.refresh();
      if (result) renderModelsView(result.models, result.health);
    } catch {}
  }

  // ===================================================================
  // Phase 3b: Model action buttons (Load / Unload / Pull)
  // ===================================================================

  document.addEventListener('click', async e => {
    const loadBtn = e.target.closest('[data-action-load]');
    if (loadBtn) {
      const name = loadBtn.getAttribute('data-action-load');
      loadBtn.classList.add('row__action--loading');
      loadBtn.textContent = 'Loading…';
      try {
        await api.loadModel(name);
        toast(`Loaded ${name}`);
        await refreshModelsView();
        refreshChatEmptyState();
        updateModelSelectorFromAPI();
      } catch (err) {
        toast(`Failed to load ${name}: ${err.message}`);
      } finally {
        loadBtn.classList.remove('row__action--loading');
        loadBtn.textContent = 'Load ▸';
      }
      return;
    }

    const unloadBtn = e.target.closest('[data-action-unload]');
    if (unloadBtn) {
      const name = unloadBtn.getAttribute('data-action-unload');
      unloadBtn.classList.add('row__action--loading');
      unloadBtn.textContent = 'Unloading…';
      try {
        await api.unloadModel(name);
        toast(`Unloaded ${name}`);
        await refreshModelsView();
        refreshChatEmptyState();
        updateModelSelectorFromAPI();
      } catch (err) {
        toast(`Failed to unload ${name}: ${err.message}`);
      } finally {
        unloadBtn.classList.remove('row__action--loading');
        unloadBtn.textContent = 'Unload';
      }
      return;
    }

    const pullBtn = e.target.closest('[data-action-pull]');
    if (pullBtn) {
      const name = pullBtn.getAttribute('data-action-pull');
      const row = pullBtn.closest('.row');
      pullBtn.classList.add('row__action--loading');
      pullBtn.textContent = 'Pulling…';

      let progressWrap = row?.querySelector('.row__progress');
      if (!progressWrap && row) {
        progressWrap = document.createElement('div');
        progressWrap.className = 'row__progress';
        progressWrap.innerHTML = '<div class="row__progress-bar" style="width:0%"></div>';
        row.appendChild(progressWrap);
      }
      const progressBar = progressWrap?.querySelector('.row__progress-bar');

      await api.pullModel(name, {
        onProgress: (data) => {
          const pct = data.percent || 0;
          if (progressBar) progressBar.style.width = `${pct}%`;
          pullBtn.textContent = `${Math.round(pct)}%`;
        },
        onComplete: async () => {
          pullBtn.classList.remove('row__action--loading');
          pullBtn.textContent = 'Load ▸';
          progressWrap?.remove();
          toast(`Downloaded ${name}`);
          await refreshModelsView();
        },
        onError: (err) => {
          pullBtn.classList.remove('row__action--loading');
          pullBtn.textContent = 'Pull ▸';
          progressWrap?.remove();
          toast(`Failed to pull ${name}: ${err.message}`);
        },
      });
      return;
    }
  });

  // ===================================================================
  // Phase 4: Chat — streaming completions
  // ===================================================================

  const conversations = new Map();
  let activeConversationId = null;
  let isStreaming = false;

  function newConversationId() {
    return 'conv-' + Date.now() + '-' + Math.random().toString(36).slice(2, 8);
  }

  function getOrCreateConversation(id) {
    if (!conversations.has(id)) {
      conversations.set(id, { id, model: currentChatModel, messages: [], title: 'New chat', created: Date.now() });
    }
    return conversations.get(id);
  }

  function setChatState(state) {
    const chat = $('.chat');
    if (!chat) return;
    chat.setAttribute('data-chat-state', state);
    const empty = $('#chat-empty');
    const thread = $('#chat-thread');
    if (state === 'empty') { if (empty) empty.hidden = false; if (thread) thread.hidden = true; }
    else { if (empty) empty.hidden = true; if (thread) thread.hidden = false; }
  }

  function setChatTitleAndStatus(model, tps) {
    $$('[data-title-model-name]').forEach(el => (el.textContent = model));
    $$('[data-status-model]').forEach(el => (el.textContent = model));
    $$('[data-status-tps]').forEach(el => (el.textContent = tps ? `${tps} tok/s` : ''));
    $$('[data-status-strip-model]').forEach(el => (el.textContent = model));
    $$('[data-status-strip-tps]').forEach(el => (el.textContent = tps ? `${tps} tok/s` : ''));
    $$('[data-thread-author]').forEach(el => (el.textContent = model));
    $$('[data-thread-tps]').forEach(el => (el.textContent = tps ? `${tps} tok/s` : ''));
  }

  function renderMessage(role, content) {
    const thread = $('#chat-thread .thread');
    if (!thread) return null;
    const article = document.createElement('article');
    article.className = `message message--${role}`;

    if (role === 'user') {
      article.innerHTML = `<div class="message__avatar">K</div>
        <div class="message__body">
          <div class="message__author">Kyle</div>
          <div class="message__content"><p>${escapeHTML(content)}</p></div>
        </div>`;
    } else {
      const modelName = currentChatModel || 'Assistant';
      article.innerHTML = `<div class="message__avatar">${modelName.charAt(0)}</div>
        <div class="message__body">
          <div class="message__author" data-thread-author>${escapeHTML(modelName)}</div>
          <details class="message__thinking" style="display:none">
            <summary>Thinking…</summary>
            <div class="message__thinking-content"></div>
          </details>
          <div class="message__content"><p>${content || ''}<span class="streaming-cursor" aria-hidden="true"></span></p></div>
          <div class="message__metrics" style="display:none">
            <span data-msg-tps></span>
            <span data-msg-ttft></span>
            <span data-msg-tokens></span>
          </div>
        </div>`;
    }

    thread.appendChild(article);
    article.scrollIntoView({ behavior: 'smooth', block: 'end' });
    return article;
  }

  function finalizeStreamingMessage(msgEl, stats) {
    if (!msgEl) return;
    const cursor = msgEl.querySelector('.streaming-cursor');
    if (cursor) cursor.remove();
    const metrics = msgEl.querySelector('.message__metrics');
    if (metrics && stats) {
      metrics.style.display = '';
      const tpsEl = metrics.querySelector('[data-msg-tps]');
      const ttftEl = metrics.querySelector('[data-msg-ttft]');
      const tokensEl = metrics.querySelector('[data-msg-tokens]');
      if (tpsEl) tpsEl.textContent = `${stats.tps} tok/s`;
      if (ttftEl) ttftEl.textContent = stats.ttft ? `${(stats.ttft / 1000).toFixed(2)}s TTFT` : '';
      if (tokensEl) tokensEl.textContent = `${stats.tokens} tokens`;
      setChatTitleAndStatus(currentChatModel, stats.tps);
    }
  }

  // markdown-it instance (loaded via CDN)
  const md = window.markdownit({
    html: false,
    linkify: true,
    typographer: true,
    breaks: true,
    highlight: function (str, lang) {
      if (lang && window.hljs && window.hljs.getLanguage(lang)) {
        try { return window.hljs.highlight(str, { language: lang }).value; } catch (_) {}
      }
      // Auto-detect if no language specified
      if (window.hljs) {
        try { return window.hljs.highlightAuto(str).value; } catch (_) {}
      }
      return ''; // fallback to default escaping
    }
  });

  function formatMessageContent(text) {
    if (!text) return '';
    return md.render(text);
  }

  async function handleSendMessage() {
    const ta = $('[data-composer-input]');
    if (!ta) return;
    const text = ta.value.trim();
    if (!text || isStreaming) return;

    if (!api.isConnected) {
      toast('Not connected to lemond — go to Connect tab to configure.');
      return;
    }
    const loadedLLMs = api.loadedModels.filter(m => m.type === 'llm');
    if (loadedLLMs.length === 0) {
      toast('No LLM loaded — load a model from the Models tab first.');
      return;
    }
    if (!activeConversationId) activeConversationId = newConversationId();

    const conv = getOrCreateConversation(activeConversationId);
    conv.model = currentChatModel;

    if (conv.messages.length === 0) {
      const thread = $('#chat-thread .thread');
      if (thread) thread.innerHTML = '';
    }

    setChatState('thread');
    conv.messages.push({ role: 'user', content: text });
    renderMessage('user', text);
    ta.value = '';
    ta.style.height = 'auto';

    const assistantEl = renderMessage('assistant', '');
    const contentDiv = assistantEl?.querySelector('.message__content');
    isStreaming = true;

    await api.chatCompletion(currentChatModel, conv.messages, {
      onReasoning: (_token, fullReasoning) => {
        const thinkingEl = assistantEl?.querySelector('.message__thinking');
        if (thinkingEl) {
          thinkingEl.style.display = '';
          thinkingEl.open = true;
          const thinkingContent = thinkingEl.querySelector('.message__thinking-content');
          if (thinkingContent) {
            thinkingContent.innerHTML = formatMessageContent(fullReasoning);
          }
          assistantEl.scrollIntoView({ behavior: 'smooth', block: 'end' });
        }
      },
      onToken: (_token, full) => {
        // Close thinking section when content starts arriving
        const thinkingEl = assistantEl?.querySelector('.message__thinking');
        if (thinkingEl && thinkingEl.open) {
          thinkingEl.open = false;
          const summary = thinkingEl.querySelector('summary');
          if (summary) summary.textContent = 'Thought process';
        }
        if (contentDiv) {
          contentDiv.innerHTML = formatMessageContent(full) + '<span class="streaming-cursor" aria-hidden="true"></span>';
          assistantEl.scrollIntoView({ behavior: 'smooth', block: 'end' });
        }
      },
      onDone: (stats) => {
        isStreaming = false;
        // Close and label thinking section
        const thinkingEl = assistantEl?.querySelector('.message__thinking');
        if (thinkingEl) {
          if (stats.reasoningTokens > 0) {
            thinkingEl.open = false;
            const summary = thinkingEl.querySelector('summary');
            if (summary) summary.textContent = `Thought for ${stats.reasoningTokens} tokens`;
          } else {
            thinkingEl.style.display = 'none';
          }
        }
        conv.messages.push({ role: 'assistant', content: stats.content });
        finalizeStreamingMessage(assistantEl, stats);
        if (conv.messages.filter(m => m.role === 'user').length === 1) {
          conv.title = text.slice(0, 50) + (text.length > 50 ? '…' : '');
          addConversationToRail(conv);
        }
      },
      onError: (err) => {
        isStreaming = false;
        if (contentP) {
          const cursor = contentP.querySelector('.streaming-cursor');
          if (cursor) cursor.remove();
          contentP.innerHTML = `<em style="color:var(--danger)">Error: ${escapeHTML(err.message)}</em>`;
        }
        toast(`Chat error: ${err.message}`);
      },
    });
  }

  function addConversationToRail(conv) {
    const railList = $('.rail__list');
    if (!railList) return;
    if (railList.querySelector(`[data-conv-id="${conv.id}"]`)) return;
    const li = document.createElement('li');
    li.className = 'rail__item';
    li.setAttribute('role', 'option');
    li.setAttribute('tabindex', '0');
    li.setAttribute('aria-selected', 'true');
    li.setAttribute('data-conv-id', conv.id);
    li.setAttribute('data-conv-model', conv.model);
    li.setAttribute('data-conv-tps', '');
    li.innerHTML = `<span class="rail__item-title">${escapeHTML(conv.title)}</span>
      <span class="rail__item-meta"><span class="rail__model-badge">${conv.model.split('-')[0].toLowerCase()}</span><span>just now</span></span>`;
    railList.querySelectorAll('.rail__item').forEach(i => i.setAttribute('aria-selected', 'false'));
    railList.insertBefore(li, railList.firstChild);
    li.addEventListener('click', () => selectConversation(li));
  }

  // ===================================================================
  // Chat: rail selection + new chat
  // ===================================================================

  function selectConversation(item) {
    if (!item) return;
    const chat = item.closest('.chat');
    if (!chat) return;
    $$('.rail__item[role="option"]').forEach(i => i.setAttribute('aria-selected', i === item ? 'true' : 'false'));
    const convId = item.dataset.convId;
    const model = item.dataset.convModel || currentChatModel;
    const tps = item.dataset.convTps || '';
    setChatState('thread');
    setChatTitleAndStatus(model, tps);

    if (conversations.has(convId)) {
      activeConversationId = convId;
      const conv = conversations.get(convId);
      currentChatModel = conv.model;
      const thread = $('#chat-thread .thread');
      if (thread) {
        thread.innerHTML = '';
        for (const msg of conv.messages) {
          const el = renderMessage(msg.role, msg.role === 'user' ? msg.content : '');
          if (msg.role === 'assistant') {
            const p = el?.querySelector('.message__content p');
            if (p) { const c = p.querySelector('.streaming-cursor'); if (c) c.remove(); p.innerHTML = formatMessageContent(msg.content); }
          }
        }
      }
      updateChatPill();
    }
  }

  function startNewChat() {
    const chat = $('.chat');
    if (!chat) return;
    $$('.rail__item[role="option"]').forEach(i => i.setAttribute('aria-selected', 'false'));
    activeConversationId = null;
    setChatState('empty');
    setChatTitleAndStatus(currentChatModel, '');
    updateChatPill();
    $('[data-composer-input]')?.focus();
  }

  $$('.rail__item[role="option"]').forEach(item => {
    item.addEventListener('click', () => selectConversation(item));
    item.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); selectConversation(item); } });
  });
  $$('[data-new-chat]').forEach(btn => btn.addEventListener('click', startNewChat));

  // ===================================================================
  // Phase 5: Backends view
  // ===================================================================

  async function refreshBackendsView() {
    try {
      const info = await api.systemInfo();
      renderBackendsView(info);
    } catch {}
  }

  function renderBackendsView(info) {
    if (!info) return;
    const banner = $('[data-backends-banner]');
    const recipes = info.recipes || {};
    let hasUpdate = false;
    let updateMsg = '';
    for (const [name, recipe] of Object.entries(recipes)) {
      for (const [, backend] of Object.entries(recipe.backends || {})) {
        if (backend.state === 'update_available') { hasUpdate = true; updateMsg = `${name} update available`; break; }
      }
      if (hasUpdate) break;
    }
    if (banner) {
      banner.style.display = hasUpdate ? '' : 'none';
      if (hasUpdate) { const t = banner.querySelector('[data-backends-banner-text]'); if (t) t.textContent = updateMsg; }
    }
  }

  // ===================================================================
  // Phase 6: Chat empty state — loaded models
  // ===================================================================

  function refreshChatEmptyState() {
    const container = $('[data-chat-loaded-models]');
    const subtitle = $('[data-hero-subtitle]');
    if (!container) return;
    const loaded = api.loadedModels;

    if (loaded.length === 0 && !api.isConnected) {
      if (subtitle) subtitle.textContent = 'Connect to your lemond server to get started.';
      container.innerHTML = `<div class="models-offline"><p class="models-offline__title">No server connected</p><p>Go to the <strong>Connect</strong> tab to configure your server URL.</p></div>`;
      return;
    }
    if (loaded.length === 0) {
      if (subtitle) subtitle.textContent = 'No models loaded. Pull and load a model from the Models tab.';
      container.innerHTML = `<div class="models-offline"><p class="models-offline__title">No models loaded</p><p>Go to the <strong>Models</strong> tab to load one.</p></div>`;
      return;
    }
    if (subtitle) subtitle.textContent = `${loaded.length} local model${loaded.length !== 1 ? 's' : ''} loaded. Pick a thread on the left, start fresh below, or try one of these.`;

    container.innerHTML = loaded.map((m, i) => {
      const name = m.model_name || 'Unknown';
      const device = (m.device || 'cpu').toUpperCase();
      const caps = labelsFor(name).map(c => badgeHTML(c)).join('');
      return `<div class="active-card">
          <div class="active-card__head"><div><div class="active-card__name">${escapeHTML(name)}</div><div class="active-card__meta">${escapeHTML(m.recipe || '')}</div></div><span class="active-card__device">${device}</span></div>
          <div class="active-card__badges">${caps}</div>
          ${i === 0 ? '<span class="active-card__status">● Active in chat</span>' : `<button class="active-card__switch" data-switch-model="${escapeHTML(name)}">Switch to ▸</button>`}
        </div>`;
    }).join('');
  }

  document.addEventListener('click', e => {
    const switchBtn = e.target.closest('[data-switch-model]');
    if (switchBtn) {
      currentChatModel = switchBtn.getAttribute('data-switch-model');
      updateChatPill();
      setChatTitleAndStatus(currentChatModel, '');
      updateModelSelectorFromAPI();
      toast(`Switched to ${currentChatModel}`);
      refreshChatEmptyState();
    }
  });

  // ===================================================================
  // Title bar model selector
  // ===================================================================

  function updateModelSelectorFromAPI() {
    const nameEl = $('[data-title-model-name]');
    const loaded = api.loadedModels.filter(m => m.type === 'llm');
    if (loaded.length > 0) {
      if (!loaded.find(m => m.model_name === currentChatModel)) {
        currentChatModel = loaded[0].model_name;
        updateChatPill();
      }
      if (nameEl) nameEl.textContent = currentChatModel;
    } else if (nameEl && !api.isConnected) {
      nameEl.textContent = 'No server';
    }
  }

  // Model selector dropdown
  const modelSelector = $('[data-title-model]');
  const modelSelectorDropdown = document.createElement('ul');
  modelSelectorDropdown.className = 'model-selector-dropdown';
  modelSelectorDropdown.hidden = true;
  if (modelSelector?.parentElement) {
    modelSelector.parentElement.style.position = 'relative';
    modelSelector.parentElement.appendChild(modelSelectorDropdown);
  }

  modelSelector?.addEventListener('click', e => {
    e.stopPropagation();
    if (!modelSelectorDropdown.hidden) { modelSelectorDropdown.hidden = true; return; }
    const loaded = api.loadedModels.filter(m => m.type === 'llm');
    if (loaded.length === 0) {
      modelSelectorDropdown.innerHTML = '<li class="model-selector-dropdown__empty">No LLMs loaded</li>';
    } else {
      modelSelectorDropdown.innerHTML = loaded.map(m => {
        const active = m.model_name === currentChatModel;
        return `<li class="model-selector-dropdown__item ${active ? 'model-selector-dropdown__item--active' : ''}" data-select-model="${escapeHTML(m.model_name)}">
          <span class="model-selector-dropdown__item-dot"></span>${escapeHTML(m.model_name)}</li>`;
      }).join('');
    }
    modelSelectorDropdown.hidden = false;
  });

  modelSelectorDropdown.addEventListener('click', e => {
    const item = e.target.closest('[data-select-model]');
    if (item) {
      currentChatModel = item.getAttribute('data-select-model');
      updateChatPill();
      setChatTitleAndStatus(currentChatModel, '');
      modelSelectorDropdown.hidden = true;
      toast(`Switched to ${currentChatModel}`);
    }
  });
  document.addEventListener('click', () => { modelSelectorDropdown.hidden = true; });

  // ===================================================================
  // Boot sequence
  // ===================================================================

  function boot() {
    renderRecipeGrid();
    renderAppliedList();
    renderRowRecipeChips();
    updateChatPill();
    initServerSettings();
    activateView('chat');

    // Initial disconnected state
    updateStatusPill('disconnected');
    updateServerSettingsStatus('disconnected');
    updateModelSelectorDot('disconnected');

    // Try connecting
    api.connect().then(connected => {
      if (connected) {
        api.models(true).then(() => {
          refreshModelsView();
          refreshChatEmptyState();
          updateModelSelectorFromAPI();
        }).catch(() => {});
      } else {
        refreshChatEmptyState();
      }
    });

    api.startPolling(15000);
  }

  boot();
})();
