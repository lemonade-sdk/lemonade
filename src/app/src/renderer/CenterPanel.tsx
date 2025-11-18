import React from 'react';

interface CenterPanelProps {
  isVisible: boolean;
}

const CenterPanel: React.FC<CenterPanelProps> = ({ isVisible }) => {
  if (!isVisible) return null;

  return (
    <div className="center-panel">
      <div className="container">
        <div className="app-suggestions-section">
          <div className="welcome-heading">Welcome!</div>
          <div className="suggestion-text">
            Use Lemonade with your favorite app
          </div>
          <div className="app-logos-grid">
            <a 
              href="https://lemonade-server.ai/docs/server/apps/open-webui/" 
              target="_blank" 
              rel="noopener noreferrer"
              className="app-logo-item" 
              title="Open WebUI"
            >
              <img 
                src="https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/openwebui.jpg" 
                alt="Open WebUI" 
                className="app-logo-img"
              />
              <span className="app-name">Open WebUI</span>
            </a>
            <a 
              href="https://lemonade-server.ai/docs/server/apps/continue/" 
              target="_blank" 
              rel="noopener noreferrer"
              className="app-logo-item" 
              title="Continue"
            >
              <img 
                src="https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/continue_dev.png" 
                alt="Continue" 
                className="app-logo-img"
              />
              <span className="app-name">Continue</span>
            </a>
            <a 
              href="https://github.com/amd/gaia" 
              target="_blank" 
              rel="noopener noreferrer"
              className="app-logo-item" 
              title="Gaia"
            >
              <img 
                src="https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/gaia.ico" 
                alt="Gaia" 
                className="app-logo-img"
              />
              <span className="app-name">Gaia</span>
            </a>
            <a 
              href="https://lemonade-server.ai/docs/server/apps/anythingLLM/" 
              target="_blank" 
              rel="noopener noreferrer"
              className="app-logo-item" 
              title="AnythingLLM"
            >
              <img 
                src="https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/anything_llm.png" 
                alt="AnythingLLM" 
                className="app-logo-img"
              />
              <span className="app-name">AnythingLLM</span>
            </a>
            <a 
              href="https://lemonade-server.ai/docs/server/apps/ai-dev-gallery/" 
              target="_blank" 
              rel="noopener noreferrer"
              className="app-logo-item" 
              title="AI Dev Gallery"
            >
              <img 
                src="https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/ai_dev_gallery.webp" 
                alt="AI Dev Gallery" 
                className="app-logo-img"
              />
              <span className="app-name">AI Dev Gallery</span>
            </a>
            <a 
              href="https://lemonade-server.ai/docs/server/apps/lm-eval/" 
              target="_blank" 
              rel="noopener noreferrer"
              className="app-logo-item" 
              title="LM-Eval"
            >
              <img 
                src="https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/lm_eval.png" 
                alt="LM-Eval" 
                className="app-logo-img"
              />
              <span className="app-name">LM-Eval</span>
            </a>
            <a 
              href="https://github.com/lemonade-sdk/lemonade-arcade" 
              target="_blank" 
              rel="noopener noreferrer"
              className="app-logo-item" 
              title="Lemonade Arcade"
            >
              <img 
                src="https://raw.githubusercontent.com/lemonade-sdk/lemonade-arcade/refs/heads/main/docs/assets/favicon.ico" 
                alt="Lemonade Arcade" 
                className="app-logo-img"
              />
              <span className="app-name">Lemonade Arcade</span>
            </a>
            <a 
              href="https://github.com/lemonade-sdk/lemonade/blob/main/docs/server/apps/ai-toolkit.md" 
              target="_blank" 
              rel="noopener noreferrer"
              className="app-logo-item" 
              title="AI Toolkit"
            >
              <img 
                src="https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/ai_toolkit.png" 
                alt="AI Toolkit" 
                className="app-logo-img"
              />
              <span className="app-name">AI Toolkit</span>
            </a>
          </div>
        </div>
      </div>
    </div>
  );
};

export default CenterPanel;

