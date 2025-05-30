from pathlib import Path
import json
from fastapi.responses import HTMLResponse
from lemonade_server.model_manager import ModelManager


def get_instructions_html(port=8000):
    """
    Show instructions on how to use the server.
    """
    # Load server models from JSON
    server_models_path = (
        Path(__file__).parent.parent.parent.parent
        / "lemonade_server"
        / "server_models.json"
    )
    with open(server_models_path, "r", encoding="utf-8") as f:
        server_models = json.load(f)

    # Use shared filter function from model_manager.py
    filtered_models = ModelManager().filter_models_by_backend(server_models)

    # Pass filtered server_models to JS
    server_models_js = (
        f"<script>window.SERVER_MODELS = {json.dumps(filtered_models)};</script>"
    )

    # New lemon-themed HTML structure
    # pylint: disable=W1401
    styled_html = f"""
    <!DOCTYPE html>
    <html lang=\"en\">
    <head>
        <meta charset=\"UTF-8\">
        <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">
        <title>Lemonade Server</title>
        <link rel="icon" href="data:,">
        <link rel=\"stylesheet\" href=\"/static/styles.css\">
        <script>
        window.SERVER_PORT = {port};
        </script>
        {server_models_js}
    </head>
    <body>
        <nav class=\"navbar\">
            <a href=\"https://github.com/lemonade-sdk/lemonade\">GitHub</a>
            <a href=\"https://lemonade-server.ai/docs/\">Docs</a>
            <a href=\"https://lemonade-server.ai/docs/server/server_models/\">Models</a>
            <a href=\"https://lemonade-server.ai/docs/server/apps/\">Featured Apps</a>
        </nav>
        <main class=\"main\">
            <div class=\"title\">🍋 Lemonade Server</div>
            <div class=\"tab-container\"> 
                <div class=\"tabs\"> 
                    <button class=\"tab active\" id=\"tab-chat\" onclick=\"showTab('chat')\">LLM Chat</button> 
                    <button class=\"tab\" id=\"tab-models\" onclick=\"showTab('models')\">Model Management</button> 
                </div> 
                <div class=\"tab-content active\" id=\"content-chat\"> 
                    <div class=\"chat-container\"> 
                        <div class=\"chat-history\" id=\"chat-history\"></div> 
                        <div class=\"chat-input-row\"> 
                            <select id=\"model-select\"></select> 
                            <input type=\"text\" id=\"chat-input\" placeholder=\"Type your message...\" /> 
                            <button id=\"send-btn\">Send</button> 
                        </div> 
                    </div> 
                </div> 
                <div class=\"tab-content\" id=\"content-models\"> 
                    <div class=\"model-mgmt-container\">
                        <div class=\"model-mgmt-pane\">
                            <h3>Installed Models</h3>
                            <table class=\"model-table\" id=\"installed-models-table\">
                                <colgroup><col style=\"width:100%\"></colgroup>
                                <tbody id=\"installed-models-tbody\"></tbody>
                            </table>
                        </div>
                        <div class=\"model-mgmt-pane\">
                            <h3>Suggested Models</h3>
                            <table class=\"model-table\" id=\"suggested-models-table\">
                                <tbody id=\"suggested-models-tbody\"></tbody>
                            </table>
                        </div>
                    </div>
                </div> 
            </div> 
        </main>
        <footer class=\"site-footer\">
            <div class=\"dad-joke\">When life gives you LLMs, make an LLM aide.</div>
            <div class=\"copyright\">Copyright 2025 AMD</div>
        </footer>
        <script src=\"https://cdn.jsdelivr.net/npm/openai@4.21.0/dist/openai.min.js\"></script> 
        <script> 
        // Tab switching logic 
        function showTab(tab) {{ 
            document.getElementById('tab-chat').classList.remove('active'); 
            document.getElementById('tab-models').classList.remove('active'); 
            document.getElementById('content-chat').classList.remove('active'); 
            document.getElementById('content-models').classList.remove('active'); 
            if (tab === 'chat') {{ 
                document.getElementById('tab-chat').classList.add('active'); 
                document.getElementById('content-chat').classList.add('active'); 
            }} else {{ 
                document.getElementById('tab-models').classList.add('active'); 
                document.getElementById('content-models').classList.add('active'); 
            }} 
        }}

        // Helper to get server base URL
        function getServerBaseUrl() {{
            const port = window.SERVER_PORT || 8000;
            return `http://localhost:{port}`;
        }}

        // Populate model dropdown from /api/v1/models endpoint
        async function loadModels() {{
            try {{
                const resp = await fetch(getServerBaseUrl() + '/api/v1/models');
                const data = await resp.json();
                const select = document.getElementById('model-select');
                select.innerHTML = '';
                if (!data.data || !Array.isArray(data.data)) {{
                    select.innerHTML = '<option>No models found (malformed response)</option>';
                    return;
                }}
                if (data.data.length === 0) {{
                    select.innerHTML = '<option>No models available</option>';
                    return;
                }}
                let defaultIndex = 0;
                data.data.forEach(function(model, index) {{
                    const modelId = model.id || model.name || model;
                    const opt = document.createElement('option');
                    opt.value = modelId;
                    opt.textContent = modelId;
                    if (modelId === 'Llama-3.2-1B-Instruct-Hybrid') {{
                        defaultIndex = index;
                    }}
                    select.appendChild(opt);
                }});
                select.selectedIndex = defaultIndex;
            }} catch (e) {{
                const select = document.getElementById('model-select');
                select.innerHTML = `<option>Error loading models: ${{e.message}}</option>`;
                console.error('Error loading models:', e);
            }}
        }}
        loadModels();

        // Model Management Tab Logic
        async function refreshModelMgmtUI() {{
            // Get installed models from /api/v1/models
            let installed = [];
            try {{
                const resp = await fetch(getServerBaseUrl() + '/api/v1/models');
                const data = await resp.json();
                if (data.data && Array.isArray(data.data)) {{
                    installed = data.data.map(m => m.id || m.name || m);
                }}
            }} catch (e) {{}}
            // All models from server_models.json (window.SERVER_MODELS)
            const allModels = window.SERVER_MODELS || {{}};
            // Filter suggested models not installed
            const suggested = Object.keys(allModels).filter(
                k => allModels[k].suggested && !installed.includes(k)
            );
            // Render installed models as a table (two columns, second is invisible)
            const installedTbody = document.getElementById('installed-models-tbody');
            installedTbody.innerHTML = '';
            installed.forEach(function(mid) {{
                var tr = document.createElement('tr');
                var tdName = document.createElement('td');
                tdName.textContent = mid;
                var tdEmpty = document.createElement('td');
                tdEmpty.style.width = '0';
                tdEmpty.style.padding = '0';
                tdEmpty.style.border = 'none';
                tr.appendChild(tdName);
                tr.appendChild(tdEmpty);
                installedTbody.appendChild(tr);
            }});
            // Render suggested models as a table
            const suggestedTbody = document.getElementById('suggested-models-tbody');
            suggestedTbody.innerHTML = '';
            suggested.forEach(mid => {{
                const tr = document.createElement('tr');
                const tdName = document.createElement('td');
                tdName.textContent = mid;
                tdName.style.paddingRight = '1em';
                tdName.style.verticalAlign = 'middle';
                const tdBtn = document.createElement('td');
                tdBtn.style.width = '1%';
                tdBtn.style.verticalAlign = 'middle';
                const btn = document.createElement('button');
                btn.textContent = '+';
                btn.title = 'Install model';
                btn.onclick = async function() {{
                    btn.disabled = true;
                    btn.textContent = 'Installing...';
                    btn.classList.add('installing-btn');
                    try {{
                        await fetch(getServerBaseUrl() + '/api/v1/pull', {{
                            method: 'POST',
                            headers: {{ 'Content-Type': 'application/json' }},
                            body: JSON.stringify({{ model_name: mid }})
                        }});
                        await refreshModelMgmtUI();
                        await loadModels(); // update chat dropdown too
                    }} catch (e) {{
                        btn.textContent = 'Error';
                    }}
                }};
                tdBtn.appendChild(btn);
                tr.appendChild(tdName);
                tr.appendChild(tdBtn);
                suggestedTbody.appendChild(tr);
            }});
        }}
        // Initial load
        refreshModelMgmtUI();
        // Optionally, refresh when switching to the tab
        document.getElementById('tab-models').addEventListener('click', refreshModelMgmtUI);

        // Chat logic (streaming with OpenAI JS client placeholder)
        const chatHistory = document.getElementById('chat-history');
        const chatInput = document.getElementById('chat-input');
        const sendBtn = document.getElementById('send-btn');
        const modelSelect = document.getElementById('model-select');
        let messages = [];

        function appendMessage(role, text) {{
            const div = document.createElement('div');
            div.className = 'chat-message ' + role;
            // Add a bubble for iMessage style
            const bubble = document.createElement('div');
            bubble.className = 'chat-bubble ' + role;
            bubble.innerHTML = text;
            div.appendChild(bubble);
            chatHistory.appendChild(div);
            chatHistory.scrollTop = chatHistory.scrollHeight;
        }}

        async function sendMessage() {{
            const text = chatInput.value.trim();
            if (!text) return;
            appendMessage('user', text);
            messages.push({{ role: 'user', content: text }});
            chatInput.value = '';
            sendBtn.disabled = true;
            // Streaming OpenAI completions (placeholder, adapt as needed)
            let llmText = '';
            appendMessage('llm', '...');
            const llmDiv = chatHistory.lastChild.querySelector('.chat-bubble.llm');
            try {{
                // Use the correct endpoint for chat completions
                const resp = await fetch(getServerBaseUrl() + '/api/v1/chat/completions', {{
                    method: 'POST',
                    headers: {{ 'Content-Type': 'application/json' }},
                    body: JSON.stringify({{
                        model: modelSelect.value,
                        messages: messages,
                        stream: true
                    }})
                }});
                if (!resp.body) throw new Error('No stream');
                const reader = resp.body.getReader();
                let decoder = new TextDecoder();
                llmDiv.textContent = '';
                while (true) {{
                    const {{ done, value }} = await reader.read();
                    if (done) break;
                    const chunk = decoder.decode(value);
                    if (chunk.trim() === 'data: [DONE]' || chunk.trim() === '[DONE]') continue;
                    // Try to extract the content from the OpenAI chunk
                    const match = chunk.match(/"content"\s*:\s*"([^"]*)"/);
                    if (match && match[1]) {{
                        llmText += match[1];
                        llmDiv.textContent = llmText;
                    }}
                }}
                messages.push({{ role: 'assistant', content: llmText }});
            }} catch (e) {{
                llmDiv.textContent = '[Error: ' + e.message + ']';
            }}
            sendBtn.disabled = false;
        }}
        sendBtn.onclick = sendMessage;
        chatInput.addEventListener('keydown', function(e) {{
            if (e.key === 'Enter') sendMessage();
        }});
        </script>
    </body>
    </html>
    """
    return HTMLResponse(content=styled_html)
