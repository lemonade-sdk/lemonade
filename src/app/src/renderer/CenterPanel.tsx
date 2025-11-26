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
    name: 'Hugging Face',
    url: 'https://huggingface.co/models?apps=lemonade&sort=trending',
    logo: 'https://nocodestartup.io/wp-content/uploads/2025/07/O-que-e-o-Hugging-Face-%E2%80%93-e-por-que-todo-projeto-moderno-de-NLP-passa-por-ele-1024x683.png',
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

