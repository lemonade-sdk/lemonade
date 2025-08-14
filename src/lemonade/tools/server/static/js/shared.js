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

// Centralized function to update the status indicator
function updateStatusIndicator(text, state = 'default') {
    const statusText = document.getElementById('model-status-text');
    const statusLight = document.getElementById('status-light');
    const indicator = document.getElementById('model-status-indicator');
    
    if (statusText) {
        statusText.textContent = text;
    }
    
    if (statusLight) {
        // Set the status light class based on state
        switch (state) {
            case 'loading':
                statusLight.className = 'status-light loading';
                break;
            case 'loaded':
            case 'online':
                statusLight.className = 'status-light online';
                break;
            case 'offline':
                statusLight.className = 'status-light offline';
                break;
            case 'error':
                statusLight.className = 'status-light offline'; // Use offline styling for errors
                break;
            default:
                statusLight.className = 'status-light';
                break;
        }
    }
    
    if (indicator) {
        // Also update the indicator container class for consistent styling
        switch (state) {
            case 'loading':
                indicator.className = 'model-status-indicator loading';
                break;
            case 'loaded':
                indicator.className = 'model-status-indicator loaded';
                break;
            case 'online':
                indicator.className = 'model-status-indicator online';
                break;
            case 'offline':
                indicator.className = 'model-status-indicator offline';
                break;
            case 'error':
                indicator.className = 'model-status-indicator offline';
                break;
            default:
                indicator.className = 'model-status-indicator';
                break;
        }
    }
}

// Make status update function globally accessible
window.updateStatusIndicator = updateStatusIndicator;

// Centralized model loading function that can be used across tabs
async function loadModelStandardized(modelId, options = {}) {
    const {
        loadButton = null,           // Optional load button to update
        onLoadingStart = null,       // Optional callback for custom loading UI
        onLoadingEnd = null,         // Optional callback for custom cleanup
        onSuccess = null,            // Optional callback on successful load
        onError = null               // Optional callback on error
    } = options;
    
    // Store original states for restoration on error
    const originalStatusText = document.getElementById('model-status-text')?.textContent || '';
    
    try {
        // Update load button if provided
        if (loadButton) {
            loadButton.disabled = true;
            loadButton.textContent = '⌛';
        }
        
        // Update status indicator to show loading state
        updateStatusIndicator(`Loading ${modelId}...`, 'loading');
        
        // Update chat dropdown and send button to show loading state
        const modelSelect = document.getElementById('model-select');
        const sendBtn = document.getElementById('send-btn');
        if (modelSelect && sendBtn) {
            // Ensure the model exists in the dropdown options
            let modelOption = modelSelect.querySelector(`option[value="${modelId}"]`);
            if (!modelOption && window.installedModels && window.installedModels.has(modelId)) {
                // Add the model to the dropdown if it doesn't exist but is installed
                modelOption = document.createElement('option');
                modelOption.value = modelId;
                modelOption.textContent = modelId;
                modelSelect.appendChild(modelOption);
            }
            
            // Set the dropdown to the new model and disable it
            if (modelOption) {
                modelSelect.value = modelId;
            }
            modelSelect.disabled = true;
            sendBtn.disabled = true;
            sendBtn.textContent = 'Loading...';
            
            // Update the loading option text
            const loadingOption = modelSelect.querySelector('option[value=""]');
            if (loadingOption) {
                loadingOption.textContent = `Loading ${modelId}...`;
            }
        }
        
        // Call custom loading start callback
        if (onLoadingStart) {
            onLoadingStart(modelId);
        }
        
        // Make the API call to load the model
        await httpRequest(getServerBaseUrl() + '/api/v1/load', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ model_name: modelId })
        });
        
        // Update model status indicator after successful load
        if (window.updateModelStatusIndicator) {
            await window.updateModelStatusIndicator();
        }
        
        // Update chat dropdown value
        if (window.updateModelSelectValue) {
            window.updateModelSelectValue();
        }
        
        // Update attachment button state
        if (window.updateAttachmentButtonState) {
            window.updateAttachmentButtonState();
        }
        
        // Reset load button if provided
        if (loadButton) {
            loadButton.disabled = false;
            loadButton.textContent = 'Load';
        }
        
        // Reset chat controls
        if (modelSelect && sendBtn) {
            modelSelect.disabled = false;
            sendBtn.disabled = false;
            sendBtn.textContent = 'Send';
            
            // Reset the default option text
            const defaultOption = modelSelect.querySelector('option[value=""]');
            if (defaultOption) {
                defaultOption.textContent = 'Pick a model';
            }
        }
        
        // Call custom loading end callback
        if (onLoadingEnd) {
            onLoadingEnd(modelId, true);
        }
        
        // Call success callback
        if (onSuccess) {
            onSuccess(modelId);
        }
        
        return true;
        
    } catch (error) {
        console.error('Error loading model:', error);
        
        // Reset load button if provided
        if (loadButton) {
            loadButton.disabled = false;
            loadButton.textContent = 'Load';
        }
        
        // Reset status indicator on error
        updateStatusIndicator(originalStatusText, 'error');
        
        // Reset chat controls on error
        const modelSelect = document.getElementById('model-select');
        const sendBtn = document.getElementById('send-btn');
        if (modelSelect && sendBtn) {
            modelSelect.disabled = false;
            sendBtn.disabled = false;
            sendBtn.textContent = 'Send';
            
            // Reset dropdown value
            if (window.updateModelSelectValue) {
                window.updateModelSelectValue();
            }
            
            // Reset the default option text
            const defaultOption = modelSelect.querySelector('option[value=""]');
            if (defaultOption) {
                defaultOption.textContent = 'Pick a model';
            }
        }
        
        // Call custom loading end callback
        if (onLoadingEnd) {
            onLoadingEnd(modelId, false);
        }
        
        // Call error callback or show default error
        if (onError) {
            onError(error, modelId);
        } else {
            showErrorBanner('Failed to load model: ' + error.message);
        }
        
        return false;
    }
}

// Make standardized load function globally accessible
window.loadModelStandardized = loadModelStandardized;

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
        // Ensure model management UI is refreshed with latest data when tab is shown
        // Use setTimeout to ensure this runs after any pending initialization
        setTimeout(() => {
            if (window.refreshModelMgmtUI) {
                window.refreshModelMgmtUI();
            }
        }, 0);
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

// Helper function to create model name with labels (moved from models.js for chat use)
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

// Initialize everything when DOM is loaded
document.addEventListener('DOMContentLoaded', function() {
    // Model status and browser management is now handled by models.js
    // This shared initialization only handles truly shared functionality
});
