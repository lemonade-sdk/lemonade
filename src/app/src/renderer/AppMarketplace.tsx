import React, { useState, useMemo } from 'react';

export interface App {
  id: string;
  name: string;
  shortDescription: string;
  fullDescription: string;
  icon: string;
  category: string;
  tags: string[];
  downloads: number;
  rating: number;
  author: string;
  license: string;
  repository?: string;
  documentation?: string;
  videoUrl?: string;
  featured?: boolean;
  installed?: boolean;
  launchUrl?: string;
}

interface AppMarketplaceProps {
  isVisible: boolean;
}

const APPS: App[] = [
  {
    id: 'open-webui',
    name: 'Open WebUI',
    shortDescription: 'User-friendly WebUI for running LLMs with advanced features',
    fullDescription: 'Open WebUI is a feature-rich and user-friendly web interface designed for running Large Language Models locally. It provides a ChatGPT-like experience with support for multiple models, conversation management, and document uploads.',
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/openwebui.jpg',
    category: 'Chat Interfaces',
    tags: ['chat', 'web-ui', 'featured', 'popular'],
    downloads: 15420,
    rating: 4.8,
    author: 'Open WebUI Team',
    license: 'MIT',
    repository: 'https://github.com/open-webui/open-webui',
    documentation: 'https://lemonade-server.ai/docs/server/apps/open-webui/',
    videoUrl: 'https://www.youtube.com/watch?v=yZs-Yzl736E',
    featured: true,
  },
  {
    id: 'continue',
    name: 'Continue',
    shortDescription: 'Open-source AI code assistant for VS Code and JetBrains',
    fullDescription: 'Continue is the leading open-source AI code assistant. Connect to any model and any context to build custom autocomplete and chat experiences inside VS Code and JetBrains IDEs.',
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/continue_dev.png',
    category: 'Development Tools',
    tags: ['coding', 'assistant', 'vscode', 'featured'],
    downloads: 12850,
    rating: 4.7,
    author: 'Continue Dev',
    license: 'Apache 2.0',
    repository: 'https://github.com/continuedev/continue',
    documentation: 'https://lemonade-server.ai/docs/server/apps/continue/',
    videoUrl: 'https://youtu.be/bP_MZnDpbUc',
    featured: true,
  },
  {
    id: 'gaia',
    name: 'Gaia',
    shortDescription: 'Run LLMs locally with ChatBot, YouTube Agent, and more',
    fullDescription: 'GAIA is an application for running LLMs locally with a beautiful interface. It includes a ChatBot, YouTube summarization agent, and other AI-powered features for enhanced productivity.',
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/gaia.ico',
    category: 'Chat Interfaces',
    tags: ['chat', 'agents', 'youtube', 'featured'],
    downloads: 8420,
    rating: 4.6,
    author: 'AMD',
    license: 'MIT',
    repository: 'https://github.com/amd/gaia',
    videoUrl: 'https://youtu.be/_PORHv_-atI',
    featured: true,
  },
  {
    id: 'anythingllm',
    name: 'AnythingLLM',
    shortDescription: 'Full-stack application for building RAG agents',
    fullDescription: 'AnythingLLM is a full-stack application that enables you to turn any document, resource, or piece of content into context that any LLM can use as references during chatting.',
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/anything_llm.png',
    category: 'AI Assistants',
    tags: ['rag', 'documents', 'agents'],
    downloads: 9240,
    rating: 4.5,
    author: 'Mintplex Labs',
    license: 'MIT',
    repository: 'https://github.com/Mintplex-Labs/anything-llm',
    documentation: 'https://lemonade-server.ai/docs/server/apps/anythingLLM/',
  },
  {
    id: 'ai-dev-gallery',
    name: 'AI Dev Gallery',
    shortDescription: "Microsoft's showcase for exploring AI capabilities",
    fullDescription: "Microsoft's AI Dev Gallery is a showcase application that helps developers explore and experiment with various AI capabilities, models, and integrations in a user-friendly interface.",
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/ai_dev_gallery.webp',
    category: 'Development Tools',
    tags: ['microsoft', 'showcase', 'development'],
    downloads: 6720,
    rating: 4.4,
    author: 'Microsoft',
    license: 'MIT',
    documentation: 'https://lemonade-server.ai/docs/server/apps/ai-dev-gallery/',
    launchUrl: 'https://aka.ms/ai-dev-gallery',
  },
  {
    id: 'lm-eval',
    name: 'LM Eval Harness',
    shortDescription: 'Framework for evaluating language models on tasks',
    fullDescription: 'A unified framework to test generative language models on a large number of different evaluation tasks. Perfect for benchmarking and comparing model performance.',
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/lm_eval.png',
    category: 'Evaluation & Testing',
    tags: ['evaluation', 'benchmarking', 'testing'],
    downloads: 5180,
    rating: 4.6,
    author: 'EleutherAI',
    license: 'MIT',
    repository: 'https://github.com/EleutherAI/lm-evaluation-harness',
    documentation: 'https://lemonade-server.ai/docs/server/apps/lm-eval/',
  },
  {
    id: 'lemonade-arcade',
    name: 'Lemonade Arcade',
    shortDescription: 'Collection of AI-powered games and demos',
    fullDescription: 'Lemonade Arcade is a collection of interactive games and demos powered by local LLMs, showcasing the creative possibilities of AI in entertainment and interactive experiences.',
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/lemonade-arcade/refs/heads/main/docs/assets/favicon.ico',
    category: 'Entertainment',
    tags: ['games', 'demo', 'interactive'],
    downloads: 4520,
    rating: 4.3,
    author: 'Lemonade SDK',
    license: 'Apache 2.0',
    repository: 'https://github.com/lemonade-sdk/lemonade-arcade',
    featured: true,
  },
  {
    id: 'ai-toolkit',
    name: 'AI Toolkit',
    shortDescription: 'VS Code extension for experimenting with LLMs',
    fullDescription: 'Microsoft AI Toolkit for Visual Studio Code helps you experiment with local LLMs, fine-tune models, and integrate AI capabilities directly into your development workflow.',
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/ai_toolkit.png',
    category: 'Development Tools',
    tags: ['vscode', 'microsoft', 'toolkit'],
    downloads: 7890,
    rating: 4.5,
    author: 'Microsoft',
    license: 'MIT',
    documentation: 'https://lemonade-server.ai/docs/server/apps/ai-toolkit/',
    videoUrl: 'https://youtu.be/JecpotOZ6qo',
  },
  {
    id: 'codegpt',
    name: 'CodeGPT',
    shortDescription: 'AI coding assistant for your IDE',
    fullDescription: 'CodeGPT is an AI-powered coding assistant that helps you write better code faster. It provides intelligent code completion, refactoring suggestions, and documentation generation.',
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/continue_dev.png',
    category: 'Development Tools',
    tags: ['coding', 'assistant', 'productivity'],
    downloads: 6340,
    rating: 4.4,
    author: 'CodeGPT',
    license: 'MIT',
    documentation: 'https://lemonade-server.ai/docs/server/apps/codeGPT/',
  },
  {
    id: 'mindcraft',
    name: 'MindCraft',
    shortDescription: 'AI agent for Minecraft using local LLMs',
    fullDescription: 'MindCraft creates intelligent Minecraft agents powered by local LLMs. Watch AI play, build, and interact in Minecraft using natural language understanding.',
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/gaia.ico',
    category: 'Entertainment',
    tags: ['minecraft', 'gaming', 'agent'],
    downloads: 3890,
    rating: 4.5,
    author: 'Kolby Thornton',
    license: 'MIT',
    repository: 'https://github.com/kolbytn/mindcraft',
    documentation: 'https://lemonade-server.ai/docs/server/apps/mindcraft/',
  },
  {
    id: 'wut',
    name: 'wut',
    shortDescription: 'Terminal assistant that explains errors',
    fullDescription: 'wut is a command-line tool that uses local LLMs to explain terminal errors in plain English. Perfect for debugging and learning from your mistakes.',
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/continue_dev.png',
    category: 'Development Tools',
    tags: ['terminal', 'debugging', 'cli'],
    downloads: 2780,
    rating: 4.3,
    author: 'shobrook',
    license: 'MIT',
    repository: 'https://github.com/shobrook/wut',
    documentation: 'https://lemonade-server.ai/docs/server/apps/wut/',
  },
  {
    id: 'openhands',
    name: 'OpenHands',
    shortDescription: 'AI coding assistant for collaborative development',
    fullDescription: 'OpenHands (formerly OpenDevin) is an autonomous AI software engineer capable of executing complex engineering tasks and collaborating actively with users on software development.',
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/continue_dev.png',
    category: 'Development Tools',
    tags: ['coding', 'assistant', 'collaboration'],
    downloads: 5670,
    rating: 4.6,
    author: 'All-Hands-AI',
    license: 'MIT',
    repository: 'https://github.com/All-Hands-AI/OpenHands',
    documentation: 'https://lemonade-server.ai/docs/server/apps/open-hands/',
  },
  {
    id: 'dify',
    name: 'Dify',
    shortDescription: 'Build node-based AI agents and RAG workflows',
    fullDescription: 'Dify is an open-source platform for building LLM apps. It combines Backend-as-a-Service and LLMOps, enabling developers to create production-ready generative AI applications.',
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/anything_llm.png',
    category: 'AI Assistants',
    tags: ['workflow', 'agents', 'rag', 'no-code'],
    downloads: 8920,
    rating: 4.7,
    author: 'Dify.AI',
    license: 'Apache 2.0',
    launchUrl: 'https://dify.ai/',
    documentation: 'https://marketplace.dify.ai/plugins/langgenius/lemonade',
  },
  {
    id: 'copilot',
    name: 'Lemonade for GitHub Copilot',
    shortDescription: 'Use Lemonade LLMs with VS Code Copilot',
    fullDescription: 'Connect your local Lemonade models to GitHub Copilot in VS Code. Get AI-powered code suggestions and completions using your own hardware.',
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/continue_dev.png',
    category: 'Development Tools',
    tags: ['vscode', 'copilot', 'coding', 'featured'],
    downloads: 11250,
    rating: 4.8,
    author: 'Lemonade SDK',
    license: 'Apache 2.0',
    launchUrl: 'https://marketplace.visualstudio.com/items?itemName=lemonade-sdk.lemonade-sdk',
    videoUrl: 'https://www.youtube.com/watch?v=HUwGxlH3yAg',
    featured: true,
  },
  {
    id: 'peel',
    name: 'PEEL',
    shortDescription: 'Using Local LLMs in Windows PowerShell',
    fullDescription: 'PEEL brings the power of local LLMs directly into your Windows PowerShell environment. Get AI assistance for scripting, automation, and system administration tasks.',
    icon: 'https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/continue_dev.png',
    category: 'Development Tools',
    tags: ['powershell', 'cli', 'automation'],
    downloads: 3240,
    rating: 4.4,
    author: 'Lemonade Apps',
    license: 'Apache 2.0',
    repository: 'https://github.com/lemonade-apps/peel',
    videoUrl: 'https://youtu.be/A-8QYktB0Io',
  },
];

const CATEGORIES = [
  'All',
  'Chat Interfaces',
  'Development Tools',
  'AI Assistants',
  'Evaluation & Testing',
  'Entertainment',
];

const SORT_OPTIONS = [
  { value: 'featured', label: 'Featured' },
  { value: 'downloads', label: 'Most Downloads' },
  { value: 'rating', label: 'Highest Rated' },
  { value: 'name', label: 'Name (A-Z)' },
];

const AppMarketplace: React.FC<AppMarketplaceProps> = ({ isVisible }) => {
  const [searchQuery, setSearchQuery] = useState('');
  const [selectedCategory, setSelectedCategory] = useState('All');
  const [sortBy, setSortBy] = useState('featured');
  const [selectedApp, setSelectedApp] = useState<App | null>(null);

  const filteredApps = useMemo(() => {
    let filtered = APPS;

    // Filter by category
    if (selectedCategory !== 'All') {
      filtered = filtered.filter(app => app.category === selectedCategory);
    }

    // Filter by search query
    if (searchQuery) {
      const query = searchQuery.toLowerCase();
      filtered = filtered.filter(app =>
        app.name.toLowerCase().includes(query) ||
        app.shortDescription.toLowerCase().includes(query) ||
        app.tags.some(tag => tag.toLowerCase().includes(query))
      );
    }

    // Sort
    filtered = [...filtered].sort((a, b) => {
      switch (sortBy) {
        case 'featured':
          return (b.featured ? 1 : 0) - (a.featured ? 1 : 0) || b.downloads - a.downloads;
        case 'downloads':
          return b.downloads - a.downloads;
        case 'rating':
          return b.rating - a.rating;
        case 'name':
          return a.name.localeCompare(b.name);
        default:
          return 0;
      }
    });

    return filtered;
  }, [searchQuery, selectedCategory, sortBy]);

  const handleInstall = (app: App) => {
    if (app.launchUrl) {
      window.open(app.launchUrl, '_blank');
    } else if (app.repository) {
      window.open(app.repository, '_blank');
    }
  };

  const handleViewDetails = (app: App) => {
    setSelectedApp(app);
  };

  const handleCloseDetails = () => {
    setSelectedApp(null);
  };

  if (!isVisible) return null;

  return (
    <div className="marketplace-container">
      {/* Header */}
      <div className="marketplace-header">
        <h1 className="marketplace-title">App Marketplace</h1>
        <p className="marketplace-subtitle">
          Discover and install applications that work with Lemonade
        </p>
      </div>

      {/* Search and Filters */}
      <div className="marketplace-controls">
        <div className="search-bar">
          <svg className="search-icon" width="16" height="16" viewBox="0 0 16 16" fill="currentColor">
            <path d="M11.742 10.344a6.5 6.5 0 1 0-1.397 1.398h-.001c.03.04.062.078.098.115l3.85 3.85a1 1 0 0 0 1.415-1.414l-3.85-3.85a1.007 1.007 0 0 0-.115-.1zM12 6.5a5.5 5.5 0 1 1-11 0 5.5 5.5 0 0 1 11 0z"/>
          </svg>
          <input
            type="text"
            placeholder="Search apps..."
            value={searchQuery}
            onChange={(e) => setSearchQuery(e.target.value)}
            className="search-input"
          />
          {searchQuery && (
            <button className="search-clear" onClick={() => setSearchQuery('')}>
              ×
            </button>
          )}
        </div>

        <div className="marketplace-filters">
          <select
            value={sortBy}
            onChange={(e) => setSortBy(e.target.value)}
            className="sort-select"
          >
            {SORT_OPTIONS.map(option => (
              <option key={option.value} value={option.value}>
                {option.label}
              </option>
            ))}
          </select>
        </div>
      </div>

      {/* Categories */}
      <div className="marketplace-categories">
        {CATEGORIES.map(category => (
          <button
            key={category}
            className={`category-button ${selectedCategory === category ? 'active' : ''}`}
            onClick={() => setSelectedCategory(category)}
          >
            {category}
          </button>
        ))}
      </div>

      {/* App Grid */}
      <div className="marketplace-content">
        {filteredApps.length === 0 ? (
          <div className="no-results">
            <p>No apps found matching your criteria</p>
          </div>
        ) : (
          <div className="app-grid">
            {filteredApps.map(app => (
              <div key={app.id} className="app-card">
                <div className="app-card-header">
                  <img src={app.icon} alt={app.name} className="app-icon" />
                  <div className="app-card-info">
                    <h3 className="app-name">{app.name}</h3>
                    <p className="app-author">{app.author}</p>
                  </div>
                  {app.featured && (
                    <span className="featured-badge">★</span>
                  )}
                </div>
                <p className="app-description">{app.shortDescription}</p>
                <div className="app-stats">
                  <span className="app-stat">
                    <svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor">
                      <path d="M8 .25a.75.75 0 0 1 .673.418l1.882 3.815 4.21.612a.75.75 0 0 1 .416 1.279l-3.046 2.97.719 4.192a.75.75 0 0 1-1.088.791L8 12.347l-3.766 1.98a.75.75 0 0 1-1.088-.79l.72-4.194L.818 6.374a.75.75 0 0 1 .416-1.28l4.21-.611L7.327.668A.75.75 0 0 1 8 .25z"/>
                    </svg>
                    {app.rating}
                  </span>
                  <span className="app-stat">
                    <svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor">
                      <path d="M.5 9.9a.5.5 0 0 1 .5.5v2.5a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-2.5a.5.5 0 0 1 1 0v2.5a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2v-2.5a.5.5 0 0 1 .5-.5z"/>
                      <path d="M7.646 11.854a.5.5 0 0 0 .708 0l3-3a.5.5 0 0 0-.708-.708L8.5 10.293V1.5a.5.5 0 0 0-1 0v8.793L5.354 8.146a.5.5 0 1 0-.708.708l3 3z"/>
                    </svg>
                    {(app.downloads / 1000).toFixed(1)}k
                  </span>
                </div>
                <div className="app-tags">
                  {app.tags.slice(0, 2).map(tag => (
                    <span key={tag} className="app-tag">{tag}</span>
                  ))}
                </div>
                <div className="app-actions">
                  <button
                    className="btn-primary"
                    onClick={() => handleInstall(app)}
                  >
                    {app.installed ? 'Launch' : 'Install'}
                  </button>
                  <button
                    className="btn-secondary"
                    onClick={() => handleViewDetails(app)}
                  >
                    Details
                  </button>
                </div>
              </div>
            ))}
          </div>
        )}
      </div>

      {/* Detail Modal */}
      {selectedApp && (
        <div className="modal-overlay" onClick={handleCloseDetails}>
          <div className="modal-content" onClick={(e) => e.stopPropagation()}>
            <button className="modal-close" onClick={handleCloseDetails}>×</button>
            
            <div className="modal-header">
              <img src={selectedApp.icon} alt={selectedApp.name} className="modal-app-icon" />
              <div className="modal-app-info">
                <h2>{selectedApp.name}</h2>
                <p className="modal-author">by {selectedApp.author}</p>
                <div className="modal-stats">
                  <span>★ {selectedApp.rating}</span>
                  <span>•</span>
                  <span>{(selectedApp.downloads / 1000).toFixed(1)}k downloads</span>
                  <span>•</span>
                  <span>{selectedApp.license}</span>
                </div>
              </div>
            </div>

            <div className="modal-body">
              <h3>Description</h3>
              <p>{selectedApp.fullDescription}</p>

              <h3>Category</h3>
              <p>{selectedApp.category}</p>

              <h3>Tags</h3>
              <div className="app-tags">
                {selectedApp.tags.map(tag => (
                  <span key={tag} className="app-tag">{tag}</span>
                ))}
              </div>

              <h3>Resources</h3>
              <div className="resource-links">
                {selectedApp.repository && (
                  <a href={selectedApp.repository} target="_blank" rel="noopener noreferrer" className="resource-link">
                    <svg width="16" height="16" viewBox="0 0 16 16" fill="currentColor">
                      <path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"/>
                    </svg>
                    Repository
                  </a>
                )}
                {selectedApp.documentation && (
                  <a href={selectedApp.documentation} target="_blank" rel="noopener noreferrer" className="resource-link">
                    <svg width="16" height="16" viewBox="0 0 16 16" fill="currentColor">
                      <path d="M1 2.828c.885-.37 2.154-.769 3.388-.893 1.33-.134 2.458.063 3.112.752v9.746c-.935-.53-2.12-.603-3.213-.493-1.18.12-2.37.461-3.287.811V2.828zm7.5-.141c.654-.689 1.782-.886 3.112-.752 1.234.124 2.503.523 3.388.893v9.923c-.918-.35-2.107-.692-3.287-.81-1.094-.111-2.278-.039-3.213.492V2.687zM8 1.783C7.015.936 5.587.81 4.287.94c-1.514.153-3.042.672-3.994 1.105A.5.5 0 0 0 0 2.5v11a.5.5 0 0 0 .707.455c.882-.4 2.303-.881 3.68-1.02 1.409-.142 2.59.087 3.223.877a.5.5 0 0 0 .78 0c.633-.79 1.814-1.019 3.222-.877 1.378.139 2.8.62 3.681 1.02A.5.5 0 0 0 16 13.5v-11a.5.5 0 0 0-.293-.455c-.952-.433-2.48-.952-3.994-1.105C10.413.809 8.985.936 8 1.783z"/>
                    </svg>
                    Documentation
                  </a>
                )}
                {selectedApp.videoUrl && (
                  <a href={selectedApp.videoUrl} target="_blank" rel="noopener noreferrer" className="resource-link">
                    <svg width="16" height="16" viewBox="0 0 16 16" fill="currentColor">
                      <path d="M8.051 1.999h.089c.822.003 4.987.033 6.11.335a2.01 2.01 0 0 1 1.415 1.42c.101.38.172.883.22 1.402l.01.104.022.26.008.104c.065.914.073 1.77.074 1.957v.075c-.001.194-.01 1.108-.082 2.06l-.008.105-.009.104c-.05.572-.124 1.14-.235 1.558a2.007 2.007 0 0 1-1.415 1.42c-1.16.312-5.569.334-6.18.335h-.142c-.309 0-1.587-.006-2.927-.052l-.17-.006-.087-.004-.171-.007-.171-.007c-1.11-.049-2.167-.128-2.654-.26a2.007 2.007 0 0 1-1.415-1.419c-.111-.417-.185-.986-.235-1.558L.09 9.82l-.008-.104A31.4 31.4 0 0 1 0 7.68v-.123c.002-.215.01-.958.064-1.778l.007-.103.003-.052.008-.104.022-.26.01-.104c.048-.519.119-1.023.22-1.402a2.007 2.007 0 0 1 1.415-1.42c.487-.13 1.544-.21 2.654-.26l.17-.007.172-.006.086-.003.171-.007A99.788 99.788 0 0 1 7.858 2h.193zM6.4 5.209v4.818l4.157-2.408L6.4 5.209z"/>
                    </svg>
                    Watch Demo
                  </a>
                )}
              </div>
            </div>

            <div className="modal-footer">
              <button className="btn-primary btn-large" onClick={() => handleInstall(selectedApp)}>
                {selectedApp.installed ? 'Launch' : 'Install'}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
};

export default AppMarketplace;

