// Model Management functionality

// Initialize model management when DOM is loaded
document.addEventListener('DOMContentLoaded', function() {
    // Initial load of model management UI
    refreshModelMgmtUI();
    
    // Refresh when switching to the models tab
    const modelsTab = document.getElementById('tab-models');
    if (modelsTab) {
        modelsTab.addEventListener('click', refreshModelMgmtUI);
    }
    
    // Set up register model form
    setupRegisterModelForm();
});

// Toggle Add Model form
function toggleAddModelForm() {
    const form = document.querySelector('.model-mgmt-register-form');
    form.classList.toggle('collapsed');
}

// Helper function to render a model table section
function renderModelTable(tbody, models, allModels, emptyMessage) {
    tbody.innerHTML = '';
    if (models.length === 0) {
        const tr = document.createElement('tr');
        const td = document.createElement('td');
        td.colSpan = 2;
        td.textContent = emptyMessage;
        td.style.textAlign = 'center';
        td.style.fontStyle = 'italic';
        td.style.color = '#666';
        td.style.padding = '1em';
        tr.appendChild(td);
        tbody.appendChild(tr);
    } else {
        models.forEach(mid => {
            const tr = document.createElement('tr');
            const tdName = document.createElement('td');
            
            tdName.appendChild(createModelNameWithLabels(mid, allModels));
            tdName.style.paddingRight = '1em';
            tdName.style.verticalAlign = 'middle';
            const tdBtn = document.createElement('td');
            tdBtn.style.width = '1%';
            tdBtn.style.verticalAlign = 'middle';
            const btn = document.createElement('button');
            btn.textContent = '+';
            btn.title = 'Install model';
            btn.onclick = async function() {
                btn.disabled = true;
                btn.textContent = 'Installing...';
                btn.classList.add('installing-btn');
                try {
                    await httpRequest(getServerBaseUrl() + '/api/v1/pull', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ model_name: mid })
                    });
                    await refreshModelMgmtUI();
                    // Update chat dropdown too if loadModels function exists
                    if (typeof loadModels === 'function') {
                        await loadModels();
                    }
                } catch (e) {
                    btn.textContent = 'Error';
                    showErrorBanner(`Failed to install model: ${e.message}`);
                }
            };
            tdBtn.appendChild(btn);
            tr.appendChild(tdName);
            tr.appendChild(tdBtn);
            tbody.appendChild(tr);
        });
    }
}

// Model Management Tab Logic
async function refreshModelMgmtUI() {
    // Get installed models from /api/v1/models
    let installed = [];
    try {
        const data = await httpJson(getServerBaseUrl() + '/api/v1/models');
        if (data.data && Array.isArray(data.data)) {
            installed = data.data.map(m => m.id || m.name || m);
        }
    } catch (e) {
        showErrorBanner(`Error loading models: ${e.message}`);
    }
    
    // All models from server_models.json (window.SERVER_MODELS)
    const allModels = window.SERVER_MODELS || {};
    
    // Separate hot models and regular suggested models not installed
    const hotModels = [];
    const regularSuggested = [];
    
    Object.keys(allModels).forEach(k => {
        if (allModels[k].suggested && !installed.includes(k)) {
            const modelData = allModels[k];
            const hasHotLabel = modelData.labels && modelData.labels.some(label => 
                label.toLowerCase() === 'hot'
            );
            
            if (hasHotLabel) {
                hotModels.push(k);
            } else {
                regularSuggested.push(k);
            }
        }
    });
    
    // Render installed models as a table (two columns, second is invisible)
    const installedTbody = document.getElementById('installed-models-tbody');
    if (installedTbody) {
        installedTbody.innerHTML = '';
        installed.forEach(function(mid) {
            var tr = document.createElement('tr');
            var tdName = document.createElement('td');
            
            tdName.appendChild(createModelNameWithLabels(mid, allModels));
            tdName.style.paddingRight = '1em';
            tdName.style.verticalAlign = 'middle';
            
            var tdBtn = document.createElement('td');
            tdBtn.style.width = '1%';
            tdBtn.style.verticalAlign = 'middle';
            const btn = document.createElement('button');
            btn.textContent = '−';
            btn.title = 'Delete model';
            btn.style.cursor = 'pointer';
            btn.onclick = async function() {
                if (!confirm(`Are you sure you want to delete the model "${mid}"?`)) {
                    return;
                }
                btn.disabled = true;
                btn.textContent = 'Deleting...';
                btn.style.backgroundColor = '#888';
                try {
                    await httpRequest(getServerBaseUrl() + '/api/v1/delete', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ model_name: mid })
                    });
                    await refreshModelMgmtUI();
                    // Update chat dropdown too if loadModels function exists
                    if (typeof loadModels === 'function') {
                        await loadModels();
                    }
                } catch (e) {
                    btn.textContent = 'Error';
                    btn.disabled = false;
                    showErrorBanner(`Failed to delete model: ${e.message}`);
                }
            };
            tdBtn.appendChild(btn);
            
            tr.appendChild(tdName);
            tr.appendChild(tdBtn);
            installedTbody.appendChild(tr);
        });
    }
    
    // Render hot models and suggested models using the helper function
    const hotTbody = document.getElementById('hot-models-tbody');
    const suggestedTbody = document.getElementById('suggested-models-tbody');
    
    if (hotTbody) {
        renderModelTable(hotTbody, hotModels, allModels, "Nice, you've already installed all these models!");
    }
    if (suggestedTbody) {
        renderModelTable(suggestedTbody, regularSuggested, allModels, "Nice, you've already installed all these models!");
    }
}

// Set up the register model form
function setupRegisterModelForm() {
    const registerForm = document.getElementById('register-model-form');
    const registerStatus = document.getElementById('register-model-status');
    
    if (registerForm && registerStatus) {
        registerForm.onsubmit = async function(e) {
            e.preventDefault();
            registerStatus.textContent = '';
            let name = document.getElementById('register-model-name').value.trim();
            
            // Always prepend 'user.' if not already present
            if (!name.startsWith('user.')) {
                name = 'user.' + name;
            }
            
            const checkpoint = document.getElementById('register-checkpoint').value.trim();
            const recipe = document.getElementById('register-recipe').value;
            const reasoning = document.getElementById('register-reasoning').checked;
            const mmproj = document.getElementById('register-mmproj').value.trim();
            
            if (!name || !recipe) { 
                return; 
            }
            
            const payload = { model_name: name, recipe, reasoning };
            if (checkpoint) payload.checkpoint = checkpoint;
            if (mmproj) payload.mmproj = mmproj;
            
            const btn = document.getElementById('register-submit');
            btn.disabled = true;
            btn.textContent = 'Installing...';
            
            try {
                await httpRequest(getServerBaseUrl() + '/api/v1/pull', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(payload)
                });
                registerStatus.textContent = 'Model installed!';
                registerStatus.style.color = '#27ae60';
                registerStatus.className = 'register-status success';
                registerForm.reset();
                await refreshModelMgmtUI();
                // Update chat dropdown too if loadModels function exists
                if (typeof loadModels === 'function') {
                    await loadModels();
                }
            } catch (e) {
                registerStatus.textContent = e.message + ' See the Lemonade Server log for details.';
                registerStatus.style.color = '#dc3545';
                registerStatus.className = 'register-status error';
                showErrorBanner(`Model install failed: ${e.message}`);
            }
            
            btn.disabled = false;
            btn.textContent = 'Install';
            refreshModelMgmtUI();
        };
    }
}
