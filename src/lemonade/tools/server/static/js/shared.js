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
    document.getElementById('content-chat').classList.remove('active'); 
    document.getElementById('content-models').classList.remove('active'); 
    if (tab === 'chat') { 
        document.getElementById('tab-chat').classList.add('active'); 
        document.getElementById('content-chat').classList.add('active');
        if (updateHash) {
            window.location.hash = 'llm-chat';
        }
    } else { 
        document.getElementById('tab-models').classList.add('active'); 
        document.getElementById('content-models').classList.add('active');
        if (updateHash) {
            window.location.hash = 'model-management';
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
    }
}

// Initialize tab based on URL hash on page load
function initializeTabFromHash() {
    const hash = window.location.hash.slice(1);
    if (hash === 'llm-chat') {
        showTab('chat', false);
    } else if (hash === 'model-management') {
        showTab('models', false);
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
