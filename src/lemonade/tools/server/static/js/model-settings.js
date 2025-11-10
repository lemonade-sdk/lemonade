// Model Settings functionality

// Model settings state
let modelSettings = {};

// === Model Settings Management ===

// Load model settings from localStorage (without DOM access)
function loadModelSettingsFromStorage() {
    const saved = localStorage.getItem('lemonade_model_settings');
    if (saved) {
        try {
            const savedSettings = JSON.parse(saved);
            modelSettings = { ...modelSettings, ...savedSettings };
        } catch (error) {
            console.error('Error loading saved settings:', error);
        }
    }
}

// Load model settings from localStorage and update UI
function loadModelSettings() {
    // First load from storage
    loadModelSettingsFromStorage();
    
    // Update UI - set values only if they exist, otherwise leave placeholder
    const tempInput = document.getElementById('setting-temperature');
    const topKInput = document.getElementById('setting-top-k');
    const topPInput = document.getElementById('setting-top-p');
    const repeatInput = document.getElementById('setting-repeat-penalty');
    const thinkingCheckbox = document.getElementById('enable-thinking');  

    
    // Check if DOM elements exist
    if (!tempInput || !topKInput || !topPInput || !repeatInput || !thinkingCheckbox) {
        return;
    }
    
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
    if (modelSettings.enable_thinking !== undefined) {
    thinkingCheckbox.checked = modelSettings.enable_thinking;
    
    } else {
    thinkingCheckbox.checked = true; // default to enabled
    
    }
}

// Auto-save model settings whenever inputs change
function setupAutoSaveSettings() {
    const inputs = ['setting-temperature', 'setting-top-k', 'setting-top-p', 'setting-repeat-penalty', 'enable-thinking'];
    
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
        const thinkingCheckbox = document.getElementById('enable-thinking');
        if (thinkingCheckbox) {
            thinkingCheckbox.addEventListener('change', function() {
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
    const thinkingCheckbox = document.getElementById('enable-thinking');
    
    // Check if DOM elements exist (might not be available if DOM isn't ready)
    if (!tempInput || !topKInput || !topPInput || !repeatInput || !thinkingCheckbox) {
        return;
    }
    
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
    modelSettings.enable_thinking = thinkingCheckbox.checked;
    
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
    document.getElementById('enable-thinking').checked = true;
    
    localStorage.removeItem('lemonade_model_settings');
}

// Get current model settings for API requests (only include non-default values)
function getCurrentModelSettings() {
    // Only update from DOM if the settings elements are available and visible
    // Otherwise, use the settings loaded from localStorage
    const tempInput = document.getElementById('setting-temperature');
    if (tempInput) {
        // DOM elements are available, update from current form state
        updateModelSettings();
    }
    
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
    if (modelSettings.enable_thinking !== undefined) {
        currentSettings.enable_thinking = modelSettings.enable_thinking;
    }
    
    console.log('getCurrentModelSettings returning:', currentSettings);
    return currentSettings;
}

// Make functions globally available for external access (like chat.js)
window.getCurrentModelSettings = getCurrentModelSettings;

// Load initial settings from localStorage immediately (without requiring DOM)
loadModelSettingsFromStorage();

// Initialize model settings when DOM is loaded
document.addEventListener('DOMContentLoaded', function() {
    // Set up model settings controls (only reset button now)
    const resetBtn = document.getElementById('reset-settings-btn');
    if (resetBtn) {
        resetBtn.onclick = resetModelSettings;
    }
    
    // Load initial model settings
    loadModelSettings();
    
    // Set up auto-save for settings
    setupAutoSaveSettings();
});
