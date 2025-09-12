// Chat logic and functionality
let messages = [];
let attachedFiles = [];
let systemMessageElement = null;

// Default model configuration
const DEFAULT_MODEL = 'Qwen2.5-0.5B-Instruct-CPU';

const THINKING_ANIM_INTERVAL_MS = 550;
// Toggle this to false if you prefer plain dots only.
const THINKING_USE_LEMON = true;
const THINKING_FRAMES = THINKING_USE_LEMON
    ? ['Thinking.','Thinking..','Thinking...','Thinking 🍋']
    : ['Thinking.','Thinking..','Thinking...'];

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

    // Display initial system message
    displaySystemMessage();
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
    const indicator = document.getElementById('model-status-indicator');
    if (indicator.classList.contains('offline') || modelSelect.value === 'server-offline') {
        modelSelect.value = 'server-offline';
    } else {
        modelSelect.innerHTML = '<option value="">Click to select a model ▼</option>';
    }
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
    const indicator = document.getElementById('model-status-indicator');
    if (currentLoadedModel && indicator.classList.contains('loading')) {
		modelSelect.value = 'loading-model';
	} else if (currentLoadedModel) {
        modelSelect.value = currentLoadedModel;
    } else if (indicator.classList.contains('offline') && modelSelect.value === 'server-offline') {
		modelSelect.value = 'server-offline';
    } else {
        return;
    }
}

// Make updateModelSelectValue accessible globally
window.updateModelSelectValue = updateModelSelectValue;

// Handle model selection change
async function handleModelSelectChange() {
    const selectedModel = modelSelect.value;

    if (!selectedModel) return; // "Click to select a model ▼" selected
    if (selectedModel === currentLoadedModel) return; // Same model already loaded

    // Use the standardized load function
    await loadModelStandardized(selectedModel, {
        onLoadingStart: (modelId) => {
            // Update dropdown to show loading state with model name
            const loadingOption = document.createElement('option');
            const select = document.getElementById('model-select');
            select.innerHTML = '';

            if (loadingOption) {
            	loadingOption.value = 'loading-model';
                loadingOption.textContent = `Loading ${modelId}...`;
            	loadingOption.hidden = true;
            	select.appendChild(loadingOption);
            }
        },
        onLoadingEnd: (modelId, success) => {
            // Reset the default option text
            const defaultOption = modelSelect.querySelector('option[value=""]');
            if (defaultOption) defaultOption.textContent = 'Click to select a model ▼';
		},
        onSuccess: () => {
            updateAttachmentButtonState();
        },
        onError: () => {
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
    } else {
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

    // Update system message when model state changes
    displaySystemMessage();
}

// Make updateAttachmentButtonState accessible globally
window.updateAttachmentButtonState = updateAttachmentButtonState;

// Make displaySystemMessage accessible globally
window.displaySystemMessage = displaySystemMessage;

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
    if (!messageToSend && attachedFiles.length === 0) return;

    // Use the standardized load function
    const success = await loadModelStandardized(DEFAULT_MODEL, {
        // Custom UI updates for auto-loading
        onLoadingStart: () => { sendBtn.textContent = 'Loading model...'; },
        // Reset send button text
        onLoadingEnd: () => { sendBtn.textContent = 'Send'; },
        // Send the message after successful load
        onSuccess: () => { sendMessage(messageToSend); },
        onError: (error) => {
            console.error('Error auto-loading default model:', error);
            showErrorBanner('Failed to load model: ' + error.message);
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
            fileAttachment.value = '';
            return;
        }
        if (!isVisionModel(currentLoadedModel)) {
            alert(`The current model "${currentLoadedModel}" does not support image inputs. Please load a model with "Vision" capabilities.`);
            fileAttachment.value = '';
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
            fileAttachment.value = '';
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
    } else if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
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
                    if (pastedText) chatInput.value = pastedText;
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
    if (pastedText) chatInput.value = pastedText;

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

    if (attachedFiles.length === 0) return;

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
        reader.onload = (e) => { thumbnail.src = e.target.result; };
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
        reader.onload = () => resolve(reader.result.split(',')[1]);
        reader.onerror = error => reject(error);
    });
}

/**
 * Incrementally (re)renders reasoning + answer without blowing away the header so user
 * collapsing/expanding persists while tokens stream.
 */
function updateMessageContent(bubbleElement, text, isMarkdown = false) {
    if (!isMarkdown) {
        bubbleElement.textContent = text;
        return;
    }

    const { main, thought, isThinking } = parseReasoningBlocks(text);

    // Pure normal markdown (no reasoning)
    if (!thought.trim()) {
        // If structure existed before, replace fully (safe—no toggle needed)
        bubbleElement.innerHTML = renderMarkdown(main);
        delete bubbleElement.dataset.thinkExpanded;
        return;
    }

    // Determine current expanded state (user preference) or default
    let expanded;
    if (bubbleElement.dataset.thinkExpanded === 'true') expanded = true;
    else if (bubbleElement.dataset.thinkExpanded === 'false') expanded = false;
    else expanded = !!isThinking; // default: open while still streaming until user intervenes

    // Create structure once
    let container = bubbleElement.querySelector('.think-tokens-container');
    let thoughtContent, headerChevron, headerLabel, mainDiv;

    if (!container) {
        bubbleElement.innerHTML = ''; // first time constructing reasoning UI

        container = document.createElement('div');
        container.className = 'think-tokens-container' + (expanded ? '' : ' collapsed');

        const header = document.createElement('div');
        header.className = 'think-tokens-header';
        header.onclick = function () { toggleThinkTokens(header); };

        headerChevron = document.createElement('span');
        headerChevron.className = 'think-tokens-chevron';
        headerChevron.textContent = expanded ? '▼' : '▶';

        headerLabel = document.createElement('span');
        headerLabel.className = 'think-tokens-label';
        header.appendChild(headerChevron);
        header.appendChild(headerLabel);

        thoughtContent = document.createElement('div');
        thoughtContent.className = 'think-tokens-content';
        thoughtContent.style.display = expanded ? 'block' : 'none';

        container.appendChild(header);
        container.appendChild(thoughtContent);
        bubbleElement.appendChild(container);

        if (main.trim()) {
            mainDiv = document.createElement('div');
            mainDiv.className = 'main-response';
            bubbleElement.appendChild(mainDiv);
        }
    } else {
        thoughtContent = container.querySelector('.think-tokens-content');
        headerChevron = container.querySelector('.think-tokens-chevron');
        headerLabel = container.querySelector('.think-tokens-label');
        mainDiv = bubbleElement.querySelector('.main-response');
    }

    // Update label & chevron (don’t override user-expanded state)
    headerChevron.textContent = expanded ? '▼' : '▶';
    // Animation-aware label handling
    if (isThinking) {
        // If not already animating, seed an initial frame then start animation
        if (bubbleElement.dataset.thinkAnimActive !== '1') {
            headerLabel.textContent = THINKING_FRAMES[0];
            startThinkingAnimation(container);
        }
    } else {
        // Stop any animation and set final label
        if (bubbleElement.dataset.thinkAnimActive === '1') {
            stopThinkingAnimation(container);
        } else {
            headerLabel.textContent = 'Thought Process';
        }
    }

    // Update reasoning content (can re-run markdown safely)
    thoughtContent.innerHTML = renderMarkdown(thought);

    // Update main answer section
    if (main.trim()) {
        if (!mainDiv) {
            mainDiv = document.createElement('div');
            mainDiv.className = 'main-response';
            bubbleElement.appendChild(mainDiv);
        }
        mainDiv.innerHTML = renderMarkdown(main);
    } else if (mainDiv) {
        mainDiv.remove();
    }

    // Persist preference
    bubbleElement.dataset.thinkExpanded = expanded ? 'true' : 'false';
}

function appendMessage(role, text, isMarkdown = false) {
    const div = document.createElement('div');
    div.className = 'chat-message ' + role;
    // Add a bubble for iMessage style
    const bubble = document.createElement('div');
    bubble.className = 'chat-bubble ' + role;

    // Check if isMarkdown is true, regardless of role
    if (isMarkdown) {
        // Build structure via incremental updater (ensures later token updates won’t wipe user toggle)
        updateMessageContent(bubble, text, true);
    } else {
        bubble.textContent = text;
    }

    div.appendChild(bubble);
    chatHistory.appendChild(div);
    chatHistory.scrollTop = chatHistory.scrollHeight;
    return bubble;
}

// Display system message based on current state
function displaySystemMessage() {
    // Remove existing system message if it exists
    if (systemMessageElement) {
        systemMessageElement.remove();
        systemMessageElement = null;
    }

    // Don't show system message if there are already user/LLM messages
    if (messages.length > 0) return;

    let messageText = '';

    // Check if any models are installed
    const hasInstalledModels = window.installedModels && window.installedModels.size > 0;

    if (!hasInstalledModels) {
        // No models installed - show first message
        messageText = `Welcome to Lemonade! To get started:
1. Head over to the Model Management tab.
2. Use the 📥Download button to download a model.
3. Use the 🚀Load button to load the model.
4. Come back to this tab, and you are ready to chat with the model.`;
    } else if (!currentLoadedModel) {
        // Models available but none loaded - show second message
        messageText = 'Welcome to Lemonade! Choose a model from the dropdown menu below to load it and start chatting.';
    }

    if (messageText) {
        const div = document.createElement('div');
        div.className = 'chat-message system';
        div.setAttribute('data-system-message', 'true');

        const bubble = document.createElement('div');
        bubble.className = 'chat-bubble system';
        bubble.textContent = messageText;

        div.appendChild(bubble);
        chatHistory.appendChild(div);
        chatHistory.scrollTop = chatHistory.scrollHeight;

        systemMessageElement = div;
    }
}

function toggleThinkTokens(header) {
    const container = header.parentElement;
    const content = container.querySelector('.think-tokens-content');
    const chevron = header.querySelector('.think-tokens-chevron');
    const bubble = header.closest('.chat-bubble');

    const nowCollapsed = !container.classList.contains('collapsed'); // current (before toggle) expanded?
    if (nowCollapsed) {
        // Collapse
        content.style.display = 'none';
        chevron.textContent = '▶';
        container.classList.add('collapsed');
        if (bubble) bubble.dataset.thinkExpanded = 'false';
    } else {
        // Expand
        content.style.display = 'block';
        chevron.textContent = '▼';
        container.classList.remove('collapsed');
        if (bubble) bubble.dataset.thinkExpanded = 'true';
    }
}

// ---------- Reasoning Parsing (Harmony + <think>) ----------

function parseReasoningBlocks(raw) {
    if (raw == null) return { main: '', thought: '', isThinking: false };
    // Added additional Harmony variants: <|channel|>analysis<|channel|>, <|channel|>analysis<|message|>, <|channel|>analysis<|assistant|>
    const RE_OPEN  = /<think>|<\|channel\|>analysis<\|(channel|message|assistant)\|>/;
    const RE_CLOSE = /<\/think>|<\|end\|>/;

    let remaining = String(raw);
    let main = '';
    let thought = '';
    let isThinking = false;

    while (true) {
        const openIdx = remaining.search(RE_OPEN);
        if (openIdx === -1) {
            if (isThinking) {
                thought += remaining;
            } else {
                main += remaining;
            }
            break;
        }

        // Text before the opener
        if (isThinking) {
            thought += remaining.slice(0, openIdx);
        } else {
            main += remaining.slice(0, openIdx);
        }

        // Drop the opener
        remaining = remaining.slice(openIdx).replace(RE_OPEN, '');
        isThinking = true;

        const closeIdx = remaining.search(RE_CLOSE);
        if (closeIdx === -1) {
            // Still streaming reasoning (no closer yet)
            thought += remaining;
            break;
        }

        // Add reasoning segment up to closer
        thought += remaining.slice(0, closeIdx);
        remaining = remaining.slice(closeIdx).replace(RE_CLOSE, '');
        isThinking = false;
        // Loop to look for additional reasoning blocks
    }
    return { main, thought, isThinking };
}

function renderMarkdownWithThinkTokens(text, preservedExpanded) {
    const { main, thought, isThinking } = parseReasoningBlocks(text);

    if (!thought.trim()) {
        return renderMarkdown(main);
    }

    // If we have a preserved user preference, honor it. Otherwise default:
    // open while streaming (original behavior) else collapsed = false.
    let expanded = (typeof preservedExpanded === 'boolean')
        ? preservedExpanded
        : !!isThinking;

    const chevron = expanded ? '▼' : '▶';
    const label = expanded && isThinking ? 'Thinking...' : (expanded ? 'Thought Process' : 'Thought Process');

    let html = `
        <div class="think-tokens-container${expanded ? '' : ' collapsed'}">
            <div class="think-tokens-header" onclick="toggleThinkTokens(this)">
                <span class="think-tokens-chevron">${chevron}</span>
                <span class="think-tokens-label">${label}</span>
            </div>
            <div class="think-tokens-content" style="display:${expanded ? 'block' : 'none'};">
                ${renderMarkdown(thought)}
            </div>
        </div>
    `;
    if (main.trim()) {
        html += `<div class="main-response">${renderMarkdown(main)}</div>`;
    }
    return html;
}

function extractAssistantReasoning(fullText) {
    const { main, thought } = parseReasoningBlocks(fullText);
    const result = { content: (main || '').trim(), raw: fullText };
    if (thought && thought.trim()) result.reasoning_content = thought.trim();
    return result;
}

// -----------------------------------------------------------

function toggleThinkTokens(header) {
    const container = header.parentElement;
    const content = container.querySelector('.think-tokens-content');
    const chevron = header.querySelector('.think-tokens-chevron');
    const bubble = header.closest('.chat-bubble');

    const nowCollapsed = !container.classList.contains('collapsed'); // current (before toggle) expanded?
    if (nowCollapsed) {
        // Collapse
        content.style.display = 'none';
        chevron.textContent = '▶';
        container.classList.add('collapsed');
        if (bubble) bubble.dataset.thinkExpanded = 'false';
    } else {
        // Expand
        content.style.display = 'block';
        chevron.textContent = '▼';
        container.classList.remove('collapsed');
        if (bubble) bubble.dataset.thinkExpanded = 'true';
    }
}

function startThinkingAnimation(container) {
    const bubble = container.closest('.chat-bubble');
    if (!bubble || bubble.dataset.thinkAnimActive === '1') return;
    const labelEl = container.querySelector('.think-tokens-label');
    if (!labelEl) return;
    bubble.dataset.thinkAnimActive = '1';
    let i = 0;
    const update = () => {
        // If streaming ended mid-cycle, stop.
        if (bubble.dataset.thinkAnimActive !== '1') return;
        labelEl.textContent = THINKING_FRAMES[i % THINKING_FRAMES.length];
        i++;
        bubble.dataset.thinkAnimId = String(setTimeout(update, THINKING_ANIM_INTERVAL_MS));
    };
    update();
}

function stopThinkingAnimation(container, finalLabel = 'Thought Process') {
    const bubble = container.closest('.chat-bubble');
    if (!bubble) return;
    bubble.dataset.thinkAnimActive = '0';
    const id = bubble.dataset.thinkAnimId;
    if (id) {
        clearTimeout(Number(id));
        delete bubble.dataset.thinkAnimId;
    }
    const labelEl = container.querySelector('.think-tokens-label');
    if (labelEl) labelEl.textContent = finalLabel;
}

async function sendMessage(existingTextIfAny) {
    const text = (existingTextIfAny !== undefined ? existingTextIfAny : chatInput.value.trim());
    if (!text && attachedFiles.length === 0) return;

    // Remove system message when user starts chatting
    if (systemMessageElement) {
        systemMessageElement.remove();
        systemMessageElement = null;
    }

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
                setTimeout(() => { successBubble.parentElement.remove(); }, 2000);
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
    if (attachedFiles.length > 0 && !isVisionModel(currentLoadedModel)) {
        alert(`Cannot send images to model "${currentLoadedModel}" as it does not support vision. Please load a model with "Vision" capabilities or remove the attached images.`);
        return;
    }

    // Create message content
    let messageContent = [];

    // Add text if present
    if (text) {
        messageContent.push({ type: "text", text: text });
    }

    // Add images if present
    if (attachedFiles.length > 0) {
        for (const file of attachedFiles) {
            if (file.type.startsWith('image/')) {
                try {
                    const base64 = await fileToBase64(file);
                    messageContent.push({
                        type: "image_url",
                        image_url: { url: `data:${file.type};base64,${base64}` }
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

    appendMessage('user', displayText, true);

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

        const reasoningEnabled = (() => {
            try {
                const meta = window.SERVER_MODELS?.[currentLoadedModel];
                return Array.isArray(meta?.labels) && meta.labels.includes('reasoning');
            } catch (_) { return false; }
        })();

        let thinkOpened = false;
        let thinkClosed = false;
        let reasoningSchemaActive = false;     // true if we saw delta.reasoning object
        let receivedAnyReasoning = false;      // true once any reasoning (schema or reasoning_content) arrived

        while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            const chunk = decoder.decode(value);
            if (!chunk.trim()) continue;

            // Handle Server-Sent Events format
            const lines = chunk.split('\n');
            for (const rawLine of lines) {
                if (!rawLine.startsWith('data: ')) continue;
                const jsonStr = rawLine.slice(6).trim();
                if (!jsonStr || jsonStr === '[DONE]') continue;

                let deltaObj;
                try { deltaObj = JSON.parse(jsonStr); } catch { continue; }
                const choiceDelta = deltaObj?.choices?.[0]?.delta;
                if (!choiceDelta) continue;

                // 1. OpenAI reasoning object (preferred schema)
                if (choiceDelta.reasoning && !thinkClosed) {
                    reasoningSchemaActive = true;
                    const r = choiceDelta.reasoning;
                    if (!thinkOpened) {
                        llmText += '<think>';
                        thinkOpened = true;
                    }
                    if (Array.isArray(r.content)) {
                        for (const seg of r.content) {
                            if (seg?.type === 'output_text' && seg.text) {
                                llmText += unescapeJsonString(seg.text);
                                receivedAnyReasoning = true;
                            }
                        }
                    }
                    if (r.done && !thinkClosed) {
                        llmText += '</think>';
                        thinkClosed = true;
                    }
                }

                // 2. llama.cpp style: reasoning_content (string fragments)
                if (choiceDelta.reasoning_content && !thinkClosed) {
                    if (!thinkOpened) {
                        llmText += '<think>';
                        thinkOpened = true;
                    }
                    llmText += unescapeJsonString(choiceDelta.reasoning_content);
                    receivedAnyReasoning = true;
                    // We DO NOT close yet; we’ll close when first normal content arrives.
                }

                // 3. Plain content tokens
                if (choiceDelta.content) {
                    let c = unescapeJsonString(choiceDelta.content);

                    // If we are inside reasoning (opened, not closed) and this is the first visible answer token,
                    // close the reasoning block before appending (unless model already emitted </think> itself).
                    if (thinkOpened && !thinkClosed) {
                        if (c.startsWith('</think>')) {
                            // Model closed it explicitly; strip that tag and mark closed
                            c = c.replace(/^<\/think>\s*/, '');
                            thinkClosed = true;
                        } else {
                            // Close ourselves (covers reasoning_content path & schema early content anomaly)
                            if (receivedAnyReasoning || reasoningEnabled) {
                                llmText += '</think>';
                                thinkClosed = true;
                            }
                        }
                    }

                    // If content stream itself begins a new reasoning section (rare), handle gracefully
                    if (!thinkOpened && /<think>/.test(c)) {
                        thinkOpened = true;
                        const parts = c.split(/<think>/);
                        // parts[0] is anything before accidental <think>, treat as normal visible content
                        llmText += parts[0];
                        // Everything after opener treated as reasoning until a closing tag or we decide to close
                        llmText += '<think>' + parts.slice(1).join('<think>');
                        receivedAnyReasoning = true;
                        updateMessageContent(llmBubble, llmText, true);
                        chatHistory.scrollTop = chatHistory.scrollHeight;
                        continue;
                    }

                    llmText += c;
                }

                updateMessageContent(llmBubble, llmText, true);
                chatHistory.scrollTop = chatHistory.scrollHeight;
            }
        }

        // Final safety close (e.g., model stopped mid-reasoning)
        if (thinkOpened && !thinkClosed) {
            llmText += '</think>';
        }

        const assistantMsg = extractAssistantReasoning(llmText);
        messages.push({ role: 'assistant', ...assistantMsg });

    } catch (e) {
        let detail = e.message;
        try {
            const errPayload = { model: currentLoadedModel, messages: messages, stream: false };
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
    // Force a final render to trigger stop animation if needed
    updateMessageContent(llmBubble, llmText, true);
}