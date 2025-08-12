// Configure MathJax
window.MathJax = {
    tex: {
        inlineMath: [['\\(', '\\)'], ['$', '$']],
        displayMath: [['\\[', '\\]'], ['$$', '$$']],
        processEscapes: true,
        processEnvironments: true
    },
    options: {
        skipHtmlTags: ['script', 'noscript', 'style', 'textarea', 'pre']
    }
};

// Configure marked.js for safe HTML rendering
marked.setOptions({
    breaks: true,
    gfm: true,
    sanitize: false,
    smartLists: true,
    smartypants: true
});

// Function to unescape JSON strings
function unescapeJsonString(str) {
    try {
        return str.replace(/\\n/g, '\n')
                 .replace(/\\t/g, '\t')
                 .replace(/\\r/g, '\r')
                 .replace(/\\"/g, '"')
                 .replace(/\\\\/g, '\\');
    } catch (error) {
        console.error('Error unescaping string:', error);
        return str;
    }
}

// Function to safely render markdown with MathJax support
function renderMarkdown(text) {
    try {
        const html = marked.parse(text);
        // Trigger MathJax to process the new content
        if (window.MathJax && window.MathJax.typesetPromise) {
            // Use a timeout to ensure DOM is updated before typesetting
            setTimeout(() => {
                window.MathJax.typesetPromise();
            }, 0);
        }
        return html;
    } catch (error) {
        console.error('Error rendering markdown:', error);
        return text; // fallback to plain text
    }
}

// Display an error message in the banner
function showErrorBanner(msg) {
    const banner = document.getElementById('error-banner');
    if (!banner) return;
    const msgEl = document.getElementById('error-banner-msg');
    const fullMsg = msg + '\nCheck the Lemonade Server logs via the system tray app for more information.';
    if (msgEl) {
        msgEl.textContent = fullMsg;
    } else {
        banner.textContent = fullMsg;
    }
    banner.style.display = 'flex';
}

function hideErrorBanner() {
    const banner = document.getElementById('error-banner');
    if (banner) banner.style.display = 'none';
}

// Helper fetch wrappers that surface server error details
async function httpRequest(url, options = {}) {
    const resp = await fetch(url, options);
    if (!resp.ok) {
        let detail = resp.statusText || 'Request failed';
        try {
            const contentType = resp.headers.get('content-type') || '';
            if (contentType.includes('application/json')) {
                const data = await resp.json();
                if (data && data.detail) detail = data.detail;
            } else {
                const text = await resp.text();
                if (text) detail = text.trim();
            }
        } catch (_) {}
        throw new Error(detail);
    }
    return resp;
}

async function httpJson(url, options = {}) {
    const resp = await httpRequest(url, options);
    return await resp.json();
}

// Tab switching logic 
function showTab(tab, updateHash = true) { 
    document.getElementById('tab-chat').classList.remove('active'); 
    document.getElementById('tab-models').classList.remove('active'); 
    document.getElementById('tab-model-settings').classList.remove('active');
    document.getElementById('content-chat').classList.remove('active'); 
    document.getElementById('content-models').classList.remove('active'); 
    document.getElementById('content-settings').classList.remove('active');
    
    if (tab === 'chat') { 
        document.getElementById('tab-chat').classList.add('active'); 
        document.getElementById('content-chat').classList.add('active');
        if (updateHash) {
            window.location.hash = 'llm-chat';
        }
    } else if (tab === 'models') { 
        document.getElementById('tab-models').classList.add('active'); 
        document.getElementById('content-models').classList.add('active');
        if (updateHash) {
            window.location.hash = 'model-management';
        }
    } else if (tab === 'settings') {
        document.getElementById('tab-model-settings').classList.add('active');
        document.getElementById('content-settings').classList.add('active');
        if (updateHash) {
            window.location.hash = 'model-settings';
        }
    }
}

// Handle hash changes for anchor navigation
function handleHashChange() {
    const hash = window.location.hash.slice(1); // Remove the # symbol
    if (hash === 'llm-chat') {
        showTab('chat', false);
    } else if (hash === 'model-management') {
        showTab('models', false);
    } else if (hash === 'model-settings') {
        showTab('settings', false);
    }
}

// Initialize tab based on URL hash on page load
function initializeTabFromHash() {
    const hash = window.location.hash.slice(1);
    if (hash === 'llm-chat') {
        showTab('chat', false);
    } else if (hash === 'model-management') {
        showTab('models', false);
    } else if (hash === 'model-settings') {
        showTab('settings', false);
    }
    // If no hash or unrecognized hash, keep default (chat tab is already active)
}

// Listen for hash changes
window.addEventListener('hashchange', handleHashChange);

// Initialize on page load
document.addEventListener('DOMContentLoaded', initializeTabFromHash);

// Handle image load failures for app logos
function handleImageFailure(img) {
    const logoItem = img.closest('.app-logo-item');
    if (logoItem) {
        logoItem.classList.add('image-failed');
    }
}

// Set up image error handlers when DOM is loaded
document.addEventListener('DOMContentLoaded', function() {
    const logoImages = document.querySelectorAll('.app-logo-img');
    logoImages.forEach(function(img) {
        let imageLoaded = false;
        
        img.addEventListener('load', function() {
            imageLoaded = true;
        });
        
        img.addEventListener('error', function() {
            if (!imageLoaded) {
                handleImageFailure(this);
            }
        });
        
        // Also check if image is already broken (cached failure)
        if (img.complete && img.naturalWidth === 0) {
            handleImageFailure(img);
        }
        
        // Timeout fallback for slow connections (5 seconds)
        setTimeout(function() {
            if (!imageLoaded && !img.complete) {
                handleImageFailure(img);
            }
        }, 5000);
    });
});

// Helper to get server base URL
function getServerBaseUrl() {
    const port = window.SERVER_PORT || 8000;
    const host = window.location.hostname || 'localhost';
    return `http://${host}:${port}`;
}

// Check if current model supports vision
function isVisionModel(modelId) {
    const allModels = window.SERVER_MODELS || {};
    const modelData = allModels[modelId];
    if (modelData && modelData.labels && Array.isArray(modelData.labels)) {
        return modelData.labels.some(label => label.toLowerCase() === 'vision');
    }
    return false;
}

// Helper function to create model name with labels
function createModelNameWithLabels(modelId, allModels) {
    // Create container for model name and labels
    const container = document.createElement('div');
    container.className = 'model-labels-container';
    
    // Add model name
    const nameSpan = document.createElement('span');
    nameSpan.textContent = modelId;
    container.appendChild(nameSpan);
    
    // Add labels if they exist
    const modelData = allModels[modelId];
    if (modelData && modelData.labels && Array.isArray(modelData.labels)) {
        modelData.labels.forEach(label => {
            const labelLower = label.toLowerCase();
            
            // Skip "hot" labels since they have their own section
            if (labelLower === 'hot') {
                return;
            }
            
            const labelSpan = document.createElement('span');
            let labelClass = 'other';
            if (labelLower === 'vision') {
                labelClass = 'vision';
            } else if (labelLower === 'embeddings') {
                labelClass = 'embeddings';
            } else if (labelLower === 'reasoning') {
                labelClass = 'reasoning';
            } else if (labelLower === 'reranking') {
                labelClass = 'reranking';
            } else if (labelLower === 'coding') {
                labelClass = 'coding';
            }
            labelSpan.className = `model-label ${labelClass}`;
            labelSpan.textContent = label;
            container.appendChild(labelSpan);
        });
    }
    
    return container;
}

// === Model Status Management ===
let currentLoadedModel = null;
let modelSettings = {};

// Check health endpoint to get current model status
async function checkModelHealth() {
    try {
        const response = await httpJson(getServerBaseUrl() + '/api/v1/health');
        return response;
    } catch (error) {
        console.error('Error checking model health:', error);
        return null;
    }
}

// Update model status indicator
async function updateModelStatusIndicator() {
    const indicator = document.getElementById('model-status-indicator');
    const statusText = document.getElementById('model-status-text');
    const unloadBtn = document.getElementById('model-unload-btn');
    
    const health = await checkModelHealth();
    const allModels = window.SERVER_MODELS || {};
    const hasInstalledModels = Object.keys(allModels).length > 0;
    
    if (health && health.model_loaded) {
        // Model is loaded
        currentLoadedModel = health.model_loaded;
        indicator.className = 'model-status-indicator loaded';
        statusText.textContent = health.model_loaded;
        unloadBtn.style.display = 'block';
        
        indicator.onclick = () => showTab('models');
    } else if (!hasInstalledModels) {
        // No models installed
        currentLoadedModel = null;
        indicator.className = 'model-status-indicator no-models';
        statusText.textContent = 'Install a Model';
        unloadBtn.style.display = 'none';
        
        indicator.onclick = () => showTab('models');
    } else {
        // Models available but none loaded
        currentLoadedModel = null;
        indicator.className = 'model-status-indicator';
        statusText.textContent = 'Load Model';
        unloadBtn.style.display = 'none';
        
        indicator.onclick = () => showTab('models');
    }
}

// Unload current model
async function unloadModel() {
    if (!currentLoadedModel) return;
    
    try {
        await httpRequest(getServerBaseUrl() + '/api/v1/unload', {
            method: 'POST'
        });
        await updateModelStatusIndicator();
    } catch (error) {
        console.error('Error unloading model:', error);
        showErrorBanner('Failed to unload model: ' + error.message);
    }
}

// === Model Browser Management ===
let currentCategory = 'hot';
let currentFilter = null;

// Toggle category in model browser (only for Hot Models now)
function toggleCategory(categoryName) {
    const header = document.querySelector(`[data-category="${categoryName}"] .category-header`);
    const content = document.getElementById(`category-${categoryName}`);
    
    if (categoryName === 'hot') {
        // Check if hot models is already selected
        const isCurrentlyActive = header.classList.contains('active');
        
        // Clear all other active states
        document.querySelectorAll('.subcategory').forEach(s => s.classList.remove('active'));
        
        if (!isCurrentlyActive) {
            // Show hot models
            header.classList.add('active');
            content.classList.add('expanded');
            currentCategory = categoryName;
            currentFilter = null;
            displayHotModels();
            updateModelBrowserTitle();
        }
        // If already active, keep it active (don't toggle off)
    }
}

// Show add model form in main area
function showAddModelForm() {
    // Clear all sidebar active states
    document.querySelectorAll('.category-header').forEach(h => h.classList.remove('active'));
    document.querySelectorAll('.category-content').forEach(c => c.classList.remove('expanded'));
    document.querySelectorAll('.subcategory').forEach(s => s.classList.remove('active'));
    
    // Highlight "Add a Model" as selected
    const addModelHeader = document.querySelector('[data-category="add"] .category-header');
    if (addModelHeader) {
        addModelHeader.classList.add('active');
    }
    
    // Hide model list and show form
    document.getElementById('model-list').style.display = 'none';
    document.getElementById('add-model-form-main').style.display = 'block';
    
    // Update title
    document.getElementById('model-browser-title').textContent = 'Add a Model';
    
    // Set current state
    currentCategory = 'add';
    currentFilter = null;
}

// Select recipe filter
function selectRecipe(recipe) {
    // Clear hot models active state
    document.querySelectorAll('.category-header').forEach(h => h.classList.remove('active'));
    document.querySelectorAll('.category-content').forEach(c => c.classList.remove('expanded'));
    
    // Clear all subcategory selections
    document.querySelectorAll('.subcategory').forEach(s => s.classList.remove('active'));
    
    // Set this recipe as active
    document.querySelector(`[data-recipe="${recipe}"]`).classList.add('active');
    
    currentCategory = 'recipes';
    currentFilter = recipe;
    displayModelsByRecipe(recipe);
    updateModelBrowserTitle();
}

// Select label filter
function selectLabel(label) {
    // Clear hot models active state
    document.querySelectorAll('.category-header').forEach(h => h.classList.remove('active'));
    document.querySelectorAll('.category-content').forEach(c => c.classList.remove('expanded'));
    
    // Clear all subcategory selections
    document.querySelectorAll('.subcategory').forEach(s => s.classList.remove('active'));
    
    // Set this label as active
    document.querySelector(`[data-label="${label}"]`).classList.add('active');
    
    currentCategory = 'labels';
    currentFilter = label;
    displayModelsByLabel(label);
    updateModelBrowserTitle();
}

// Update model browser title
function updateModelBrowserTitle() {
    const title = document.getElementById('model-browser-title');
    
    if (currentCategory === 'hot') {
        title.textContent = 'Hot Models';
    } else if (currentCategory === 'recipes') {
        title.textContent = `Recipe: ${currentFilter}`;
    } else if (currentCategory === 'labels') {
        title.textContent = `Category: ${currentFilter}`;
    } else {
        title.textContent = 'Models';
    }
}

// Display suggested models (Qwen3-0.6B-GGUF as default)
function displaySuggestedModels() {
    const modelList = document.getElementById('model-list');
    const allModels = window.SERVER_MODELS || {};
    
    modelList.innerHTML = '';
    
    // First show Qwen3-0.6B-GGUF as the default suggested model
    if (allModels['Qwen3-0.6B-GGUF']) {
        createModelItem('Qwen3-0.6B-GGUF', allModels['Qwen3-0.6B-GGUF'], modelList);
    }
    
    // Then show other suggested models (excluding the one already shown)
    Object.entries(allModels).forEach(([modelId, modelData]) => {
        if (modelData.suggested && modelId !== 'Qwen3-0.6B-GGUF') {
            createModelItem(modelId, modelData, modelList);
        }
    });
    
    if (modelList.innerHTML === '') {
        modelList.innerHTML = '<p>No suggested models available</p>';
    }
}

// Display hot models
function displayHotModels() {
    const modelList = document.getElementById('model-list');
    const addModelForm = document.getElementById('add-model-form-main');
    const allModels = window.SERVER_MODELS || {};
    
    // Show model list, hide form
    modelList.style.display = 'block';
    addModelForm.style.display = 'none';
    
    modelList.innerHTML = '';
    
    Object.entries(allModels).forEach(([modelId, modelData]) => {
        if (modelData.labels && modelData.labels.includes('hot')) {
            createModelItem(modelId, modelData, modelList);
        }
    });
}

// Display models by recipe
function displayModelsByRecipe(recipe) {
    const modelList = document.getElementById('model-list');
    const addModelForm = document.getElementById('add-model-form-main');
    const allModels = window.SERVER_MODELS || {};
    
    // Show model list, hide form
    modelList.style.display = 'block';
    addModelForm.style.display = 'none';
    
    modelList.innerHTML = '';
    
    Object.entries(allModels).forEach(([modelId, modelData]) => {
        if (modelData.recipe === recipe) {
            createModelItem(modelId, modelData, modelList);
        }
    });
}

// Display models by label
function displayModelsByLabel(label) {
    const modelList = document.getElementById('model-list');
    const addModelForm = document.getElementById('add-model-form-main');
    const allModels = window.SERVER_MODELS || {};
    
    // Show model list, hide form
    modelList.style.display = 'block';
    addModelForm.style.display = 'none';
    
    modelList.innerHTML = '';
    
    Object.entries(allModels).forEach(([modelId, modelData]) => {
        if (label === 'custom') {
            // Show user-added models (those starting with 'user.')
            if (modelId.startsWith('user.')) {
                createModelItem(modelId, modelData, modelList);
            }
        } else if (modelData.labels && modelData.labels.includes(label)) {
            createModelItem(modelId, modelData, modelList);
        }
    });
}

// Create model item element
function createModelItem(modelId, modelData, container) {
    const item = document.createElement('div');
    item.className = 'model-item';
    
    const info = document.createElement('div');
    info.className = 'model-item-info';
    
    const name = document.createElement('div');
    name.className = 'model-item-name';
    name.appendChild(createModelNameWithLabels(modelId, window.SERVER_MODELS || {}));
    
    info.appendChild(name);
    
    // Only add description if it exists and is not empty
    if (modelData.description && modelData.description.trim()) {
        const description = document.createElement('div');
        description.className = 'model-item-description';
        description.textContent = modelData.description;
        info.appendChild(description);
    }
    
    const actions = document.createElement('div');
    actions.className = 'model-item-actions';
    
    // Check if model is installed (this would need to be determined from server state)
    const isInstalled = true; // Placeholder - would check against installed models
    const isLoaded = currentLoadedModel === modelId;
    
    if (!isInstalled) {
        const installBtn = document.createElement('button');
        installBtn.className = 'model-item-btn install';
        installBtn.textContent = 'Install';
        installBtn.onclick = () => installModel(modelId);
        actions.appendChild(installBtn);
    } else {
        if (isLoaded) {
            const unloadBtn = document.createElement('button');
            unloadBtn.className = 'model-item-btn unload';
            unloadBtn.textContent = 'Unload';
            unloadBtn.onclick = () => unloadModel();
            actions.appendChild(unloadBtn);
        } else {
            const loadBtn = document.createElement('button');
            loadBtn.className = 'model-item-btn load';
            loadBtn.textContent = 'Load';
            loadBtn.onclick = () => loadModel(modelId);
            actions.appendChild(loadBtn);
        }
        
        const deleteBtn = document.createElement('button');
        deleteBtn.className = 'model-item-btn delete';
        deleteBtn.textContent = 'Delete';
        deleteBtn.onclick = () => deleteModel(modelId);
        actions.appendChild(deleteBtn);
    }
    
    item.appendChild(info);
    item.appendChild(actions);
    container.appendChild(item);
}

// Install model
async function installModel(modelId) {
    try {
        const modelData = window.SERVER_MODELS[modelId];
        await httpRequest(getServerBaseUrl() + '/api/v1/pull', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ model_name: modelId, ...modelData })
        });
        // Refresh model list
        if (currentCategory === 'hot') displayHotModels();
        else if (currentCategory === 'recipes') displayModelsByRecipe(currentFilter);
        else if (currentCategory === 'labels') displayModelsByLabel(currentFilter);
    } catch (error) {
        console.error('Error installing model:', error);
        showErrorBanner('Failed to install model: ' + error.message);
    }
}

// Load model
async function loadModel(modelId) {
    try {
        await httpRequest(getServerBaseUrl() + '/api/v1/load', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ model_name: modelId })
        });
        await updateModelStatusIndicator();
        // Refresh model list
        if (currentCategory === 'hot') displayHotModels();
        else if (currentCategory === 'recipes') displayModelsByRecipe(currentFilter);
        else if (currentCategory === 'labels') displayModelsByLabel(currentFilter);
    } catch (error) {
        console.error('Error loading model:', error);
        showErrorBanner('Failed to load model: ' + error.message);
    }
}

// Delete model
async function deleteModel(modelId) {
    if (!confirm(`Are you sure you want to delete the model "${modelId}"?`)) {
        return;
    }
    
    try {
        await httpRequest(getServerBaseUrl() + '/api/v1/delete', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ model_name: modelId })
        });
        // Refresh model list
        if (currentCategory === 'hot') displayHotModels();
        else if (currentCategory === 'recipes') displayModelsByRecipe(currentFilter);
        else if (currentCategory === 'labels') displayModelsByLabel(currentFilter);
    } catch (error) {
        console.error('Error deleting model:', error);
        showErrorBanner('Failed to delete model: ' + error.message);
    }
}

// === Model Settings Management ===

// Load model settings from localStorage or set to empty for defaults
function loadModelSettings() {
    const saved = localStorage.getItem('lemonade_model_settings');
    if (saved) {
        try {
            const savedSettings = JSON.parse(saved);
            modelSettings = { ...modelSettings, ...savedSettings };
        } catch (error) {
            console.error('Error loading saved settings:', error);
        }
    }
    
    // Update UI - set values only if they exist, otherwise leave placeholder
    const tempInput = document.getElementById('setting-temperature');
    const topKInput = document.getElementById('setting-top-k');
    const topPInput = document.getElementById('setting-top-p');
    const repeatInput = document.getElementById('setting-repeat-penalty');
    
    // Load saved values or leave as placeholder "default"
    if (modelSettings.temperature !== undefined) {
        tempInput.value = modelSettings.temperature;
    }
    if (modelSettings.top_k !== undefined) {
        topKInput.value = modelSettings.top_k;
    }
    if (modelSettings.top_p !== undefined) {
        topPInput.value = modelSettings.top_p;
    }
    if (modelSettings.repeat_penalty !== undefined) {
        repeatInput.value = modelSettings.repeat_penalty;
    }
}

// Auto-save model settings whenever inputs change
function setupAutoSaveSettings() {
    const inputs = ['setting-temperature', 'setting-top-k', 'setting-top-p', 'setting-repeat-penalty'];
    
    inputs.forEach(inputId => {
        const input = document.getElementById(inputId);
        if (input) {
            input.addEventListener('input', function() {
                updateModelSettings();
            });
            input.addEventListener('blur', function() {
                updateModelSettings();
            });
        }
    });
}

// Update model settings from current input values
function updateModelSettings() {
    const tempInput = document.getElementById('setting-temperature');
    const topKInput = document.getElementById('setting-top-k');
    const topPInput = document.getElementById('setting-top-p');
    const repeatInput = document.getElementById('setting-repeat-penalty');
    
    // Only set values if user has entered something, otherwise use undefined (default)
    modelSettings = {};
    
    if (tempInput.value && tempInput.value.trim() !== '') {
        modelSettings.temperature = parseFloat(tempInput.value);
    }
    if (topKInput.value && topKInput.value.trim() !== '') {
        modelSettings.top_k = parseInt(topKInput.value);
    }
    if (topPInput.value && topPInput.value.trim() !== '') {
        modelSettings.top_p = parseFloat(topPInput.value);
    }
    if (repeatInput.value && repeatInput.value.trim() !== '') {
        modelSettings.repeat_penalty = parseFloat(repeatInput.value);
    }
    
    // Save to localStorage
    localStorage.setItem('lemonade_model_settings', JSON.stringify(modelSettings));
}

// Reset model settings to defaults (clear all inputs)
function resetModelSettings() {
    modelSettings = {};
    
    // Clear all input values to show placeholders
    document.getElementById('setting-temperature').value = '';
    document.getElementById('setting-top-k').value = '';
    document.getElementById('setting-top-p').value = '';
    document.getElementById('setting-repeat-penalty').value = '';
    
    localStorage.removeItem('lemonade_model_settings');
}

// Get current model settings for API requests (only include non-default values)
function getCurrentModelSettings() {
    // Update settings from current form state before returning
    updateModelSettings();
    
    // Return only the settings that have actual values (not defaults)
    const currentSettings = {};
    if (modelSettings.temperature !== undefined) {
        currentSettings.temperature = modelSettings.temperature;
    }
    if (modelSettings.top_k !== undefined) {
        currentSettings.top_k = modelSettings.top_k;
    }
    if (modelSettings.top_p !== undefined) {
        currentSettings.top_p = modelSettings.top_p;
    }
    if (modelSettings.repeat_penalty !== undefined) {
        currentSettings.repeat_penalty = modelSettings.repeat_penalty;
    }
    
    return currentSettings;
}

// Initialize everything when DOM is loaded
document.addEventListener('DOMContentLoaded', function() {
    // Set up model status indicator
    updateModelStatusIndicator();
    setInterval(updateModelStatusIndicator, 5000); // Check every 5 seconds
    
    // Set up model status controls
    document.getElementById('model-unload-btn').onclick = unloadModel;
    
    // Set up model settings controls (only reset button now)
    document.getElementById('reset-settings-btn').onclick = resetModelSettings;
    
    // Load initial model settings
    loadModelSettings();
    
    // Set up auto-save for settings
    setupAutoSaveSettings();
    
    // Initialize model browser with hot models
    displayHotModels();
});

// Make functions globally available
window.toggleCategory = toggleCategory;
window.selectRecipe = selectRecipe;
window.selectLabel = selectLabel;
window.showAddModelForm = showAddModelForm;
