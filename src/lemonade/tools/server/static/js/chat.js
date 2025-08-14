// Chat logic and functionality
let messages = [];
let attachedFiles = [];

// Default model configuration
const DEFAULT_MODEL = 'Qwen2.5-0.5B-Instruct-CPU';

// Get DOM elements
let chatHistory, chatInput, sendBtn, attachmentBtn, fileAttachment, attachmentsPreviewContainer, attachmentsPreviewRow, modelSelect;

// Initialize chat functionality when DOM is loaded
document.addEventListener('DOMContentLoaded', function() {
    chatHistory = document.getElementById('chat-history');
    chatInput = document.getElementById('chat-input');
    sendBtn = document.getElementById('send-btn');
    attachmentBtn = document.getElementById('attachment-btn');
    fileAttachment = document.getElementById('file-attachment');
    attachmentsPreviewContainer = document.getElementById('attachments-preview-container');
    attachmentsPreviewRow = document.getElementById('attachments-preview-row');
    modelSelect = document.getElementById('model-select');

    // Set up event listeners
    setupChatEventListeners();
    
    // Initialize model dropdown (will be populated when models.js calls updateModelStatusIndicator)
    initializeModelDropdown();
    
    // Update attachment button state periodically
    updateAttachmentButtonState();
    setInterval(updateAttachmentButtonState, 1000);
});

function setupChatEventListeners() {
    // Send button click
    sendBtn.onclick = sendMessage;
    
    // Attachment button click
    attachmentBtn.onclick = () => {
        if (!currentLoadedModel) {
            alert('Please load a model first before attaching images.');
            return;
        }
        if (!isVisionModel(currentLoadedModel)) {
            alert(`The current model "${currentLoadedModel}" does not support image inputs. Please load a model with "Vision" capabilities to attach images.`);
            return;
        }
        fileAttachment.click();
    };

    // File input change
    fileAttachment.addEventListener('change', handleFileSelection);

    // Chat input events
    chatInput.addEventListener('keydown', handleChatInputKeydown);
    chatInput.addEventListener('paste', handleChatInputPaste);
    
    // Model select change
    modelSelect.addEventListener('change', handleModelSelectChange);
    
    // Send button click
    sendBtn.addEventListener('click', function() {
        // Check if we have a loaded model
        if (currentLoadedModel && modelSelect.value !== '' && !modelSelect.disabled) {
            sendMessage();
        } else if (!currentLoadedModel) {
            // Auto-load default model and send
            autoLoadDefaultModelAndSend();
        }
    });
}

// Initialize model dropdown with available models
function initializeModelDropdown() {
    const allModels = window.SERVER_MODELS || {};
    
    // Clear existing options except the first one
    modelSelect.innerHTML = '<option value="">Pick a model</option>';
    
    // Add only installed models to dropdown
    Object.keys(allModels).forEach(modelId => {
        // Only add if the model is installed
        if (window.installedModels && window.installedModels.has(modelId)) {
            const option = document.createElement('option');
            option.value = modelId;
            option.textContent = modelId;
            modelSelect.appendChild(option);
        }
    });
    
    // Set current selection based on loaded model
    updateModelSelectValue();
}

// Make dropdown initialization accessible globally so models.js can refresh it
window.initializeModelDropdown = initializeModelDropdown;

// Update model select value to match currently loaded model
function updateModelSelectValue() {
    if (currentLoadedModel) {
        modelSelect.value = currentLoadedModel;
    } else {
        modelSelect.value = '';
    }
}

// Make updateModelSelectValue accessible globally
window.updateModelSelectValue = updateModelSelectValue;

// Handle model selection change
async function handleModelSelectChange() {
    const selectedModel = modelSelect.value;
    
    if (!selectedModel) {
        return; // "Pick a model" selected
    }
    
    if (selectedModel === currentLoadedModel) {
        return; // Same model already loaded
    }
    
    // Use the standardized load function
    await loadModelStandardized(selectedModel, {
        onLoadingStart: (modelId) => {
            // Update dropdown to show loading state with model name
            const loadingOption = modelSelect.querySelector('option[value=""]');
            if (loadingOption) {
                loadingOption.textContent = `Loading ${modelId}...`;
            }
        },
        onLoadingEnd: (modelId, success) => {
            // Reset the default option text
            const defaultOption = modelSelect.querySelector('option[value=""]');
            if (defaultOption) {
                defaultOption.textContent = 'Pick a model';
            }
        },
        onSuccess: (loadedModelId) => {
            // Update attachment button state for new model
            updateAttachmentButtonState();
        },
        onError: (error, failedModelId) => {
            // Reset dropdown to previous value on error
            updateModelSelectValue();
        }
    });
}

// Update attachment button state based on current model
function updateAttachmentButtonState() {
    // Update model dropdown selection
    updateModelSelectValue();
    
    // Update send button state based on model loading
    if (modelSelect.disabled) {
        sendBtn.disabled = true;
        sendBtn.textContent = 'Loading...';
    } else {
        sendBtn.disabled = false;
        sendBtn.textContent = 'Send';
    }
    
    if (!currentLoadedModel) {
        attachmentBtn.style.opacity = '0.5';
        attachmentBtn.style.cursor = 'not-allowed';
        attachmentBtn.title = 'Load a model first';
        return;
    }
    
    const isVision = isVisionModel(currentLoadedModel);
    
    if (isVision) {
        attachmentBtn.style.opacity = '1';
        attachmentBtn.style.cursor = 'pointer';
        attachmentBtn.title = 'Attach images';
    } else {
        attachmentBtn.style.opacity = '0.5';
        attachmentBtn.style.cursor = 'not-allowed';
        attachmentBtn.title = 'Image attachments not supported by this model';
    }
}

// Make updateAttachmentButtonState accessible globally
window.updateAttachmentButtonState = updateAttachmentButtonState;

// Auto-load default model and send message
async function autoLoadDefaultModelAndSend() {
    // Check if default model is available and installed
    if (!window.SERVER_MODELS || !window.SERVER_MODELS[DEFAULT_MODEL]) {
        showErrorBanner('No models available. Please install a model first.');
        return;
    }
    
    if (!window.installedModels || !window.installedModels.has(DEFAULT_MODEL)) {
        showErrorBanner('Default model is not installed. Please install it from the Model Management tab.');
        return;
    }
    
    // Store the message to send after loading
    const messageToSend = chatInput.value.trim();
    if (!messageToSend && attachedFiles.length === 0) {
        return; // Nothing to send
    }
    
    // Use the standardized load function
    const success = await loadModelStandardized(DEFAULT_MODEL, {
        onLoadingStart: (modelId) => {
            // Custom UI updates for auto-loading
            sendBtn.textContent = 'Loading model...';
        },
        onLoadingEnd: (modelId, loadSuccess) => {
            // Reset send button text
            sendBtn.textContent = 'Send';
        },
        onSuccess: (loadedModelId) => {
            // Send the message after successful load
            sendMessage(messageToSend);
        },
        onError: (error, failedModelId) => {
            console.error('Error auto-loading default model:', error);
        }
    });
}

// Check if model supports vision and update attachment button
function checkCurrentModel() {
    if (attachedFiles.length > 0 && currentLoadedModel && !isVisionModel(currentLoadedModel)) {
        if (confirm(`The current model "${currentLoadedModel}" does not support images. Would you like to remove the attached images?`)) {
            clearAttachments();
        }
    }
    updateAttachmentButtonState();
}

// Handle file selection
function handleFileSelection() {
    if (fileAttachment.files.length > 0) {
        // Check if current model supports vision
        if (!currentLoadedModel) {
            alert('Please load a model first before attaching images.');
            fileAttachment.value = ''; // Clear the input
            return;
        }
        if (!isVisionModel(currentLoadedModel)) {
            alert(`The current model "${currentLoadedModel}" does not support image inputs. Please load a model with "Vision" capabilities.`);
            fileAttachment.value = ''; // Clear the input
            return;
        }
        
        // Filter only image files
        const imageFiles = Array.from(fileAttachment.files).filter(file => {
            if (!file.type.startsWith('image/')) {
                console.warn(`Skipping non-image file: ${file.name} (${file.type})`);
                return false;
            }
            return true;
        });
        
        if (imageFiles.length === 0) {
            alert('Please select only image files (PNG, JPG, GIF, etc.)');
            fileAttachment.value = ''; // Clear the input
            return;
        }
        
        if (imageFiles.length !== fileAttachment.files.length) {
            alert(`${fileAttachment.files.length - imageFiles.length} non-image file(s) were skipped. Only image files are supported.`);
        }
        
        attachedFiles = imageFiles;
        updateInputPlaceholder();
        updateAttachmentPreviewVisibility();
        updateAttachmentPreviews();
    }
}

// Handle chat input keydown events
function handleChatInputKeydown(e) {
    if (e.key === 'Escape' && attachedFiles.length > 0) {
        e.preventDefault();
        clearAttachments();
    } else if (e.key === 'Enter') {
        // Check if we have a loaded model
        if (currentLoadedModel && modelSelect.value !== '' && !modelSelect.disabled) {
            sendMessage();
        } else if (!currentLoadedModel) {
            // Auto-load default model and send
            autoLoadDefaultModelAndSend();
        }
    }
}

// Handle paste events for images
async function handleChatInputPaste(e) {
    e.preventDefault();
    
    const clipboardData = e.clipboardData || window.clipboardData;
    const items = clipboardData.items;
    let hasImage = false;
    let pastedText = '';
    
    // Check for text content first
    for (let item of items) {
        if (item.type === 'text/plain') {
            pastedText = clipboardData.getData('text/plain');
        }
    }
    
    // Check for images
    for (let item of items) {
        if (item.type.indexOf('image') !== -1) {
            hasImage = true;
            const file = item.getAsFile();
            if (file && file.type.startsWith('image/')) {
                // Check if current model supports vision before adding image
                const currentModel = modelSelect.value;
                if (!isVisionModel(currentModel)) {
                    alert(`The selected model "${currentModel}" does not support image inputs. Please select a model with "Vision" capabilities to paste images.`);
                    // Only paste text, skip the image
                    if (pastedText) {
                        chatInput.value = pastedText;
                    }
                    return;
                }
                // Add to attachedFiles array only if it's an image and model supports vision
                attachedFiles.push(file);
            } else if (file) {
                console.warn(`Skipping non-image pasted file: ${file.name || 'unknown'} (${file.type})`);
            }
        }
    }
    
    // Update input box content - only show text, images will be indicated separately
    if (pastedText) {
        chatInput.value = pastedText;
    }
    
    // Update placeholder to show attached images
    updateInputPlaceholder();
    updateAttachmentPreviewVisibility();
    updateAttachmentPreviews();
}

function clearAttachments() {
    attachedFiles = [];
    fileAttachment.value = '';
    updateInputPlaceholder();
    updateAttachmentPreviewVisibility();
    updateAttachmentPreviews();
}

function updateAttachmentPreviewVisibility() {
    if (attachedFiles.length > 0) {
        attachmentsPreviewContainer.classList.add('has-attachments');
    } else {
        attachmentsPreviewContainer.classList.remove('has-attachments');
    }
}

function updateAttachmentPreviews() {
    // Clear existing previews
    attachmentsPreviewRow.innerHTML = '';
    
    if (attachedFiles.length === 0) {
        return;
    }
    
    attachedFiles.forEach((file, index) => {
        // Skip non-image files (extra safety check)
        if (!file.type.startsWith('image/')) {
            console.warn(`Skipping non-image file in preview: ${file.name} (${file.type})`);
            return;
        }
        
        const previewDiv = document.createElement('div');
        previewDiv.className = 'attachment-preview';
        
        // Create thumbnail
        const thumbnail = document.createElement('img');
        thumbnail.className = 'attachment-thumbnail';
        thumbnail.alt = file.name;
        
        // Create filename display
        const filename = document.createElement('div');
        filename.className = 'attachment-filename';
        filename.textContent = file.name || `pasted-image-${index + 1}`;
        filename.title = file.name || `pasted-image-${index + 1}`;
        
        // Create remove button
        const removeBtn = document.createElement('button');
        removeBtn.className = 'attachment-remove-btn';
        removeBtn.innerHTML = '✕';
        removeBtn.title = 'Remove this image';
        removeBtn.onclick = () => removeAttachment(index);
        
        // Generate thumbnail for image
        const reader = new FileReader();
        reader.onload = (e) => {
            thumbnail.src = e.target.result;
        };
        reader.readAsDataURL(file);
        
        previewDiv.appendChild(thumbnail);
        previewDiv.appendChild(filename);
        previewDiv.appendChild(removeBtn);
        attachmentsPreviewRow.appendChild(previewDiv);
    });
}

function removeAttachment(index) {
    attachedFiles.splice(index, 1);
    updateInputPlaceholder();
    updateAttachmentPreviewVisibility();
    updateAttachmentPreviews();
}

// Function to update input placeholder to show attached files
function updateInputPlaceholder() {
    if (attachedFiles.length > 0) {
        chatInput.placeholder = `Type your message... (${attachedFiles.length} image${attachedFiles.length > 1 ? 's' : ''} attached)`;
    } else {
        chatInput.placeholder = 'Type your message...';
    }
}

// Function to convert file to base64
function fileToBase64(file) {
    return new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.readAsDataURL(file);
        reader.onload = () => resolve(reader.result.split(',')[1]); // Remove data:image/...;base64, prefix
        reader.onerror = error => reject(error);
    });
}

function appendMessage(role, text, isMarkdown = false) {
    const div = document.createElement('div');
    div.className = 'chat-message ' + role;
    // Add a bubble for iMessage style
    const bubble = document.createElement('div');
    bubble.className = 'chat-bubble ' + role;
    
    if (role === 'llm' && isMarkdown) {
        bubble.innerHTML = renderMarkdownWithThinkTokens(text);
    } else {
        bubble.textContent = text;
    }
    
    div.appendChild(bubble);
    chatHistory.appendChild(div);
    chatHistory.scrollTop = chatHistory.scrollHeight;
    return bubble; // Return the bubble element for streaming updates
}

function updateMessageContent(bubbleElement, text, isMarkdown = false) {
    if (isMarkdown) {
        bubbleElement.innerHTML = renderMarkdownWithThinkTokens(text);
    } else {
        bubbleElement.textContent = text;
    }
}

function renderMarkdownWithThinkTokens(text) {
    // Check if text contains opening think tag
    if (text.includes('<think>')) {
        if (text.includes('</think>')) {
            // Complete think block - handle as before
            const thinkMatch = text.match(/<think>(.*?)<\/think>/s);
            if (thinkMatch) {
                const thinkContent = thinkMatch[1].trim();
                const mainResponse = text.replace(/<think>.*?<\/think>/s, '').trim();
                
                // Create collapsible structure
                let html = '';
                if (thinkContent) {
                    html += `
                        <div class="think-tokens-container">
                            <div class="think-tokens-header" onclick="toggleThinkTokens(this)">
                                <span class="think-tokens-chevron">▼</span>
                                <span class="think-tokens-label">Thinking...</span>
                            </div>
                            <div class="think-tokens-content">
                                ${renderMarkdown(thinkContent)}
                            </div>
                        </div>
                    `;
                }
                if (mainResponse) {
                    html += `<div class="main-response">${renderMarkdown(mainResponse)}</div>`;
                }
                return html;
            }
        } else {
            // Partial think block - only opening tag found, still being generated
            const thinkMatch = text.match(/<think>(.*)/s);
            if (thinkMatch) {
                const thinkContent = thinkMatch[1];
                const beforeThink = text.substring(0, text.indexOf('<think>'));
                
                let html = '';
                if (beforeThink.trim()) {
                    html += `<div class="main-response">${renderMarkdown(beforeThink)}</div>`;
                }
                
                html += `
                    <div class="think-tokens-container">
                        <div class="think-tokens-header" onclick="toggleThinkTokens(this)">
                            <span class="think-tokens-chevron">▼</span>
                            <span class="think-tokens-label">Thinking...</span>
                        </div>
                        <div class="think-tokens-content">
                            ${renderMarkdown(thinkContent)}
                        </div>
                    </div>
                `;
                
                return html;
            }
        }
    }
    
    // Fallback to normal markdown rendering
    return renderMarkdown(text);
}

function toggleThinkTokens(header) {
    const container = header.parentElement;
    const content = container.querySelector('.think-tokens-content');
    const chevron = header.querySelector('.think-tokens-chevron');
    
    if (content.style.display === 'none') {
        content.style.display = 'block';
        chevron.textContent = '▼';
        container.classList.remove('collapsed');
    } else {
        content.style.display = 'none';
        chevron.textContent = '▶';
        container.classList.add('collapsed');
    }
}

async function sendMessage() {
    const text = chatInput.value.trim();
    if (!text && attachedFiles.length === 0) return;
    
    // Check if a model is loaded, if not, automatically load the default model
    if (!currentLoadedModel) {
        const allModels = window.SERVER_MODELS || {};
        
        if (allModels[DEFAULT_MODEL]) {
            try {
                // Show loading message
                const loadingBubble = appendMessage('system', 'Loading default model, please wait...');
                
                // Load the default model
                await httpRequest(getServerBaseUrl() + '/api/v1/load', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ model_name: DEFAULT_MODEL })
                });
                
                // Update model status
                await updateModelStatusIndicator();
                
                // Remove loading message
                loadingBubble.parentElement.remove();
                
                // Show success message briefly
                const successBubble = appendMessage('system', `Loaded ${DEFAULT_MODEL} successfully!`);
                setTimeout(() => {
                    successBubble.parentElement.remove();
                }, 2000);
                
            } catch (error) {
                alert('Please load a model first before sending messages.');
                return;
            }
        } else {
            alert('Please load a model first before sending messages.');
            return;
        }
    }
    
    // Check if trying to send images to non-vision model
    if (attachedFiles.length > 0) {
        if (!isVisionModel(currentLoadedModel)) {
            alert(`Cannot send images to model "${currentLoadedModel}" as it does not support vision. Please load a model with "Vision" capabilities or remove the attached images.`);
            return;
        }
    }
    
    // Create message content
    let messageContent = [];
    
    // Add text if present
    if (text) {
        messageContent.push({
            type: "text",
            text: text
        });
    }
    
    // Add images if present
    if (attachedFiles.length > 0) {
        for (const file of attachedFiles) {
            if (file.type.startsWith('image/')) {
                try {
                    const base64 = await fileToBase64(file);
                    messageContent.push({
                        type: "image_url",
                        image_url: {
                            url: `data:${file.type};base64,${base64}`
                        }
                    });
                } catch (error) {
                    console.error('Error converting image to base64:', error);
                }
            }
        }
    }
    
    // Display user message (show text and file names)
    let displayText = text;
    if (attachedFiles.length > 0) {
        const fileNames = attachedFiles.map(f => f.name || 'pasted-image').join(', ');
        displayText = displayText ? `${displayText}\n[Images: ${fileNames}]` : `[Images: ${fileNames}]`;
    }
    
    appendMessage('user', displayText);
    
    // Add to messages array
    const userMessage = {
        role: 'user',
        content: messageContent.length === 1 && messageContent[0].type === "text" 
            ? messageContent[0].text 
            : messageContent
    };
    messages.push(userMessage);
    
    // Clear input and attachments
    chatInput.value = '';
    attachedFiles = [];
    fileAttachment.value = '';
    updateInputPlaceholder(); // Reset placeholder
    updateAttachmentPreviewVisibility(); // Hide preview container
    updateAttachmentPreviews(); // Clear previews
    sendBtn.disabled = true;
    
    // Streaming OpenAI completions (placeholder, adapt as needed)
    let llmText = '';
    const llmBubble = appendMessage('llm', '...');
    try {
        // Use the correct endpoint for chat completions with model settings
        const modelSettings = getCurrentModelSettings ? getCurrentModelSettings() : {};
        console.log('Applying model settings to API request:', modelSettings);
        
        const payload = {
            model: currentLoadedModel,
            messages: messages,
            stream: true,
            ...modelSettings // Apply current model settings
        };
        
        const resp = await httpRequest(getServerBaseUrl() + '/api/v1/chat/completions', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        });
        if (!resp.body) throw new Error('No stream');
        const reader = resp.body.getReader();
        let decoder = new TextDecoder();
        llmBubble.textContent = '';
        while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            const chunk = decoder.decode(value);
            if (chunk.trim() === 'data: [DONE]' || chunk.trim() === '[DONE]') continue;
            
            // Handle Server-Sent Events format
            const lines = chunk.split('\n');
            for (const line of lines) {
                if (line.startsWith('data: ')) {
                    const jsonStr = line.substring(6).trim();
                    if (jsonStr === '[DONE]') continue;
                    
                    try {
                        const delta = JSON.parse(jsonStr);
                        if (delta.choices && delta.choices[0] && delta.choices[0].delta) {
                            const content = delta.choices[0].delta.content;
                            if (content) {
                                llmText += unescapeJsonString(content);
                                updateMessageContent(llmBubble, llmText, true);
                                chatHistory.scrollTop = chatHistory.scrollHeight;
                            }
                        }
                    } catch (parseErr) {
                        console.warn('Failed to parse JSON:', jsonStr, parseErr);
                    }
                }
            }
        }
        if (!llmText) throw new Error('No response');
        
        // Split assistant response into content and reasoning_content so llama.cpp's Jinja does not need to parse <think> tags
        function splitAssistantResponse(text) {
            const THINK_OPEN = '<think>';
            const THINK_CLOSE = '</think>';
            const result = { content: text };
            const start = text.indexOf(THINK_OPEN);
            const end = text.indexOf(THINK_CLOSE);
            if (start !== -1 && end !== -1 && end > start) {
                const reasoning = text.substring(start + THINK_OPEN.length, end).trim();
                const visible = (text.substring(0, start) + text.substring(end + THINK_CLOSE.length)).trim();
                if (reasoning) result.reasoning_content = reasoning;
                result.content = visible;
            }
            return result;
        }

        const assistantMsg = splitAssistantResponse(llmText);
        messages.push({ role: 'assistant', ...assistantMsg });
    } catch (e) {
        let detail = e.message;
        try {
            const errPayload = { ...payload, stream: false };
            const errResp = await httpJson(getServerBaseUrl() + '/api/v1/chat/completions', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(errPayload)
            });
            if (errResp && errResp.detail) detail = errResp.detail;
        } catch (_) {}
        llmBubble.textContent = '[Error: ' + detail + ']';
        showErrorBanner(`Chat error: ${detail}`);
    }
    sendBtn.disabled = false;
}
