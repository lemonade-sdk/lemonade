const API_URL = 'http://localhost:5000';

// Check backend status on load
async function checkBackendStatus() {
    const statusIndicator = document.getElementById('status');
    const statusText = document.getElementById('status-text');
    
    try {
        const response = await fetch(`${API_URL}/health`);
        const data = await response.json();
        
        if (data.status === 'ok') {
            statusIndicator.classList.add('connected');
            statusText.textContent = 'Connected to TypeScript backend';
        }
    } catch (error) {
        statusIndicator.classList.add('error');
        statusText.textContent = 'Backend connection failed';
        console.error('Backend health check failed:', error);
    }
}

// Test button handler
document.getElementById('test-btn').addEventListener('click', async () => {
    const responseBox = document.getElementById('response');
    responseBox.classList.add('show');
    responseBox.textContent = 'Connecting to backend...';
    
    try {
        const response = await fetch(`${API_URL}/api/test`);
        const data = await response.json();
        
        responseBox.textContent = JSON.stringify(data, null, 2);
    } catch (error) {
        responseBox.textContent = `Error: ${error.message}`;
        console.error('API test failed:', error);
    }
});

// Check status when page loads
window.addEventListener('DOMContentLoaded', () => {
    // Give backend a moment to fully start
    setTimeout(checkBackendStatus, 1500);
});

