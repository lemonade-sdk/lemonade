// Chat logic and functionality
let messages = [];
let attachedFiles = [];

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
    
    // Load models for the dropdown
    loadModels();
});

function setupChatEventListeners() {
    // Send button click
    sendBtn.onclick = sendMessage;
    
    // Attachment button click
    attachmentBtn.onclick = () => {
        const currentModel = modelSelect.value;
        if (!isVisionModel(currentModel)) {
            alert(`The selected model "${currentModel}" does not support image inputs. Please select a model with "Vision" capabilities to attach images.`);
            return;
        }
        fileAttachment.click();
    };

    // File input change
    fileAttachment.addEventListener('change', handleFileSelection);

    // Chat input events
    chatInput.addEventListener('keydown', handleChatInputKeydown);
    chatInput.addEventListener('paste', handleChatInputPaste);

    // Model selection change
    modelSelect.addEventListener('change', handleModelChange);
}

// Update attachment button state based on current model
function updateAttachmentButtonState() {
    const currentModel = modelSelect.value;
    const isVision = isVisionModel(currentModel);
    
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

// Handle model selection change
function handleModelChange() {
    const currentModel = modelSelect.value;
    updateAttachmentButtonState(); // Update button visual state
    
    if (attachedFiles.length > 0 && !isVisionModel(currentModel)) {
        if (confirm(`The selected model "${currentModel}" does not support images. Would you like to remove the attached images?`)) {
            clearAttachments();
        } else {
            // Find a vision model to switch back to
            const allModels = window.SERVER_MODELS || {};
            const visionModels = Array.from(modelSelect.options).filter(option => 
                isVisionModel(option.value)
            );
            
            if (visionModels.length > 0) {
                modelSelect.value = visionModels[0].value;
                updateAttachmentButtonState(); // Update button state again
                alert(`Switched back to "${visionModels[0].value}" which supports images.`);
            } else {
                alert('No vision models available. Images will be cleared.');
                clearAttachments();
            }
        }
    }
}

// Populate model dropdown from /api/v1/models endpoint
async function loadModels() {
    try {
        const data = await httpJson(getServerBaseUrl() + '/api/v1/models');
        const select = document.getElementById('model-select');
        select.innerHTML = '';
        if (!data.data || !Array.isArray(data.data)) {
            select.innerHTML = '<option>No models found (malformed response)</option>';
            return;
        }
        if (data.data.length === 0) {
            select.innerHTML = '<option>No models available</option>';
            return;
        }
        
        // Filter out embedding models from chat interface
        const allModels = window.SERVER_MODELS || {};
        let filteredModels = [];
        let defaultIndex = 0;
        
        // Check if model is specified in URL parameters
        const urlModel = new URLSearchParams(window.location.search).get('model');
        let urlModelIndex = -1;
        
        data.data.forEach(function(model, index) {
            const modelId = model.id || model.name || model;
            const modelInfo = allModels[modelId] || {};
            const labels = modelInfo.labels || [];
            
            // Skip models with "embeddings" or "reranking" label
            if (labels.includes('embeddings') || labels.includes('reranking')) {
                return;
            }
            
            filteredModels.push(modelId);
            const opt = document.createElement('option');
            opt.value = modelId;
            opt.textContent = modelId;
            
            // Check if this model matches the URL parameter
            if (urlModel && modelId === urlModel) {
                urlModelIndex = filteredModels.length - 1;
            }
            
            // Default fallback for backwards compatibility
            if (modelId === 'Llama-3.2-1B-Instruct-Hybrid') {
                defaultIndex = filteredModels.length - 1;
            }
            
            select.appendChild(opt);
        });
        
        if (filteredModels.length === 0) {
            select.innerHTML = '<option>No chat models available</option>';
            return;
        }
        
        // Select the URL-specified model if found, otherwise use default
        if (urlModelIndex !== -1) {
            select.selectedIndex = urlModelIndex;
            console.log(`Selected model from URL parameter: ${urlModel}`);
        } else {
            select.selectedIndex = defaultIndex;
            if (urlModel) {
                console.warn(`Model '${urlModel}' specified in URL not found in available models`);
            }
        }
        
        // Update attachment button state after model is loaded
        updateAttachmentButtonState();
    } catch (e) {
        const select = document.getElementById('model-select');
        select.innerHTML = `<option>Error loading models: ${e.message}</option>`;
        console.error('Error loading models:', e);
        showErrorBanner(`Error loading models: ${e.message}`);
    }
}

// Handle file selection
function handleFileSelection() {
    if (fileAttachment.files.length > 0) {
        // Check if current model supports vision
        const currentModel = modelSelect.value;
        if (!isVisionModel(currentModel)) {
            alert(`The selected model "${currentModel}" does not support image inputs. Please select a model with "Vision" capabilities or choose a different model.`);
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
        sendMessage();
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
    
    // Check if trying to send images to non-vision model
    if (attachedFiles.length > 0) {
        const currentModel = modelSelect.value;
        if (!isVisionModel(currentModel)) {
            alert(`Cannot send images to model "${currentModel}" as it does not support vision. Please select a model with "Vision" capabilities or remove the attached images.`);
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
        // Use the correct endpoint for chat completions
        const payload = {
            model: modelSelect.value,
            messages: messages,
            stream: true
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
