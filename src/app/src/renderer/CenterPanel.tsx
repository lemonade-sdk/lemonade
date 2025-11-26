import React from 'react';

interface CenterPanelProps {
  isVisible: boolean;
}

const apps = [
  {
    name: 'Open WebUI',
    url: 'https://lemonade-server.ai/docs/server/apps/open-webui/',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/openwebui.jpg',
  },
  {
    name: 'Continue',
    url: 'https://lemonade-server.ai/docs/server/apps/continue/',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/continue_dev.png',
  },
  {
    name: 'n8n',
    url: 'https://n8n.io/integrations/lemonade-model/',
    logo: 'https://avatars.githubusercontent.com/u/45487711?s=48&v=4',
  },
  {
    name: 'Gaia',
    url: 'https://github.com/amd/gaia',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/gaia.ico',
  },
  {
    name: 'AnythingLLM',
    url: 'https://lemonade-server.ai/docs/server/apps/anythingLLM/',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/anything_llm.png',
  },
  {
    name: 'AI Dev Gallery',
    url: 'https://lemonade-server.ai/docs/server/apps/ai-dev-gallery/',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/ai_dev_gallery.webp',
  },
  {
    name: 'LM-Eval',
    url: 'https://lemonade-server.ai/docs/server/apps/lm-eval/',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/lm_eval.png',
  },
  {
    name: 'Infinity Arcade',
    url: 'https://github.com/lemonade-sdk/infinity-arcade',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/lemonade-arcade/refs/heads/main/docs/assets/favicon.ico',
  },
  {
    name: 'AI Toolkit',
    url: 'https://github.com/lemonade-sdk/lemonade/blob/main/docs/server/apps/ai-toolkit.md',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/ai_toolkit.png',
  },
];

const CenterPanel: React.FC<CenterPanelProps> = ({ isVisible }) => {
  if (!isVisible) return null;

  // Duplicate apps for seamless infinite scroll
  const scrollApps = [...apps, ...apps];

  return (
    <div className="center-panel">
      <div className="marketplace-section">
        <div className="marketplace-badge">
          <span className="badge-icon">✦</span>
          <span className="badge-text">Coming Soon</span>
        </div>
        <h1 className="marketplace-title">App Marketplace</h1>
        <p className="marketplace-subtitle">
          One-click install for your favorite AI apps
        </p>
        
        <div className="apps-gallery-container">
          <div className="apps-gallery">
            {scrollApps.map((app, index) => (
              <a
                key={index}
                href={app.url}
                target="_blank"
                rel="noopener noreferrer"
                className="gallery-app-item"
                title={app.name}
              >
                <div className="gallery-app-icon">
                  <img src={app.logo} alt={app.name} />
                </div>
                <span className="gallery-app-name">{app.name}</span>
              </a>
            ))}
          </div>
        </div>
      </div>
    </div>
  );
};

export default CenterPanel;

