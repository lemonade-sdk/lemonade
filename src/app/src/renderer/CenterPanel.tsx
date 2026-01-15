import React, { useRef, useState, useEffect, useCallback } from 'react';

interface CenterPanelProps {
  isVisible: boolean;
  onClose?: () => void;
}

const apps = [
  {
    name: 'Hugging Face',
    url: 'https://huggingface.co/models?apps=lemonade&sort=trending',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/hugging_face.png',
  },
  {
    name: 'Continue',
    url: 'https://lemonade-server.ai/docs/server/apps/continue/',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/continue.png',
  },
  {
    name: 'n8n',
    url: 'https://n8n.io/integrations/lemonade-model/',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/n8n.png',
  },
  {
    name: 'Gaia',
    url: 'https://github.com/amd/gaia',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/gaia.png',
  },
  {
    name: 'Infinity Arcade',
    url: 'https://github.com/lemonade-sdk/infinity-arcade',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/infinity_arcade.png',
  },
  {
    name: 'GitHub Copilot',
    url: 'https://marketplace.visualstudio.com/items?itemName=GitHub.copilot',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/github_copilot.png',
  },
  {
    name: 'OpenHands',
    url: 'https://openhands.dev/',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/openhands.png',
  },
  {
    name: 'Dify',
    url: 'https://marketplace.dify.ai/plugins/langgenius/lemonade',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/dify.png',
  },
  {
    name: 'Deep Tutor',
    url: 'https://deeptutor.knowhiz.us/',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/deep_tutor.png',
  },
  {
    name: 'Iterate.ai',
    url: 'https://www.iterate.ai/',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/iterate_ai.png',
  },
  {
    name: 'Perplexica',
    url: 'https://github.com/ItzCrazyKns/Perplexica',
    logo: 'https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/perplexica.png',
  },
];

const CenterPanel: React.FC<CenterPanelProps> = ({ isVisible, onClose }) => {
  const galleryRef = useRef<HTMLDivElement>(null);
  const containerRef = useRef<HTMLDivElement>(null);
  
  // Drag state
  const [isDragging, setIsDragging] = useState(false);
  const [startX, setStartX] = useState(0);
  const [scrollLeft, setScrollLeft] = useState(0);
  const [velocity, setVelocity] = useState(0);
  const [isSpinning, setIsSpinning] = useState(false);
  const [translateX, setTranslateX] = useState(0);
  const [hasInteracted, setHasInteracted] = useState(false);
  const [isHovering, setIsHovering] = useState(false);
  
  // Track mouse positions for velocity calculation
  const lastMouseX = useRef(0);
  const lastTime = useRef(0);
  const velocityHistory = useRef<number[]>([]);
  const animationFrame = useRef<number | null>(null);
  const hasDragged = useRef(false);
  const clickTarget = useRef<HTMLAnchorElement | null>(null);

  // Duplicate apps for seamless infinite scroll
  const scrollApps = [...apps, ...apps, ...apps];

  // Get the width of one set of apps for looping
  const getSetWidth = useCallback(() => {
    if (galleryRef.current) {
      return galleryRef.current.scrollWidth / 3;
    }
    return 0;
  }, []);

  // Handle infinite loop wrapping
  const wrapPosition = useCallback((pos: number) => {
    const setWidth = getSetWidth();
    if (setWidth === 0) return pos;
    
    // Keep position within bounds of -setWidth to 0 for seamless loop
    while (pos < -setWidth * 2) pos += setWidth;
    while (pos > 0) pos -= setWidth;
    
    return pos;
  }, [getSetWidth]);

  // Constant drift speed (negative = scroll left)
  const driftSpeed = -0.4;

  // Animation loop - always runs, pauses on hover or drag
  useEffect(() => {
    // Pause when dragging or hovering
    if (isDragging || isHovering) return;

    const friction = 0.92;

    const animate = () => {
      setVelocity(prev => {
        // Blend towards drift speed instead of zero
        const diff = prev - driftSpeed;
        const newVelocity = driftSpeed + diff * friction;
        
        setTranslateX(pos => wrapPosition(pos + newVelocity));
        return newVelocity;
      });
      
      animationFrame.current = requestAnimationFrame(animate);
    };

    animationFrame.current = requestAnimationFrame(animate);

    return () => {
      if (animationFrame.current) {
        cancelAnimationFrame(animationFrame.current);
      }
    };
  }, [isDragging, isHovering, wrapPosition]);

  const handleMouseDown = (e: React.MouseEvent) => {
    // Store the target to check later if it was a link
    const target = e.target as HTMLElement;
    const linkElement = target.tagName === 'A' ? target as HTMLAnchorElement : target.closest('a');
    clickTarget.current = linkElement;
    
    if (animationFrame.current) {
      cancelAnimationFrame(animationFrame.current);
    }
    
    setHasInteracted(true);
    setIsDragging(true);
    setIsSpinning(false);
    setStartX(e.clientX);
    setScrollLeft(translateX);
    lastMouseX.current = e.clientX;
    lastTime.current = Date.now();
    velocityHistory.current = [];
    hasDragged.current = false;
  };

  const handleMouseMove = useCallback((e: MouseEvent) => {
    if (!isDragging) return;

    const x = e.clientX;
    const walk = x - startX;
    
    // Mark as dragged if moved more than a few pixels
    if (Math.abs(walk) > 5) {
      hasDragged.current = true;
    }
    
    const newTranslate = wrapPosition(scrollLeft + walk);
    setTranslateX(newTranslate);

    // Calculate velocity
    const now = Date.now();
    const dt = now - lastTime.current;
    if (dt > 0) {
      const dx = x - lastMouseX.current;
      const currentVelocity = dx / dt * 16; // Normalize to ~60fps
      velocityHistory.current.push(currentVelocity);
      
      // Keep only last 5 velocity samples
      if (velocityHistory.current.length > 5) {
        velocityHistory.current.shift();
      }
    }
    
    lastMouseX.current = x;
    lastTime.current = now;
  }, [isDragging, startX, scrollLeft, wrapPosition]);

  const handleMouseUp = useCallback(() => {
    if (!isDragging) return;
    
    setIsDragging(false);

    // If user clicked on a link and didn't drag, navigate to it
    if (!hasDragged.current && clickTarget.current) {
      window.open(clickTarget.current.href, '_blank', 'noopener,noreferrer');
      clickTarget.current = null;
      return;
    }
    
    clickTarget.current = null;

    // Calculate average velocity from history
    if (velocityHistory.current.length > 0) {
      const avgVelocity = velocityHistory.current.reduce((a, b) => a + b, 0) / velocityHistory.current.length;
      
      // Apply velocity boost if significant, otherwise just let drift take over
      if (Math.abs(avgVelocity) > 2) {
        setVelocity(avgVelocity * 1.5); // Boost for more satisfying spin
      }
    }
    // Always trigger spinning state so the animation loop continues
    setIsSpinning(true);
  }, [isDragging]);

  // Handle click prevention when dragging
  const handleClick = useCallback((e: React.MouseEvent) => {
    if (hasDragged.current) {
      e.preventDefault();
      e.stopPropagation();
    }
  }, []);

  // Global mouse events for smooth dragging
  useEffect(() => {
    if (isDragging) {
      window.addEventListener('mousemove', handleMouseMove);
      window.addEventListener('mouseup', handleMouseUp);
      
      return () => {
        window.removeEventListener('mousemove', handleMouseMove);
        window.removeEventListener('mouseup', handleMouseUp);
      };
    }
  }, [isDragging, handleMouseMove, handleMouseUp]);

  if (!isVisible) return null;

  return (
    <div className="center-panel">
      {onClose && (
        <button 
          className="center-panel-close-btn" 
          onClick={onClose}
          title="Close panel"
        >
          ×
        </button>
      )}
      <div className="marketplace-section">
        <div className="marketplace-badge">
          <span className="badge-icon">✦</span>
          <span className="badge-text">Coming Soon</span>
        </div>
        <h1 className="marketplace-title">App Marketplace</h1>
        <p className="marketplace-subtitle">
          Quick start for your favorite AI apps
        </p>
        
        <div 
          ref={containerRef}
          className={`apps-gallery-container ${hasInteracted ? 'interactive' : ''}`}
          onMouseDown={handleMouseDown}
          onMouseEnter={() => setIsHovering(true)}
          onMouseLeave={() => setIsHovering(false)}
        >
          <div 
            ref={galleryRef}
            className={`apps-gallery ${isDragging ? 'dragging' : ''}`}
            style={{
              transform: `translateX(${translateX}px)`,
              cursor: isDragging ? 'grabbing' : 'grab',
            }}
          >
            {scrollApps.map((app, index) => (
              <a
                key={index}
                href={app.url}
                target="_blank"
                rel="noopener noreferrer"
                className="gallery-app-item"
                title={app.name}
                onClick={handleClick}
                draggable={false}
              >
                <div className="gallery-app-icon">
                  <img src={app.logo} alt={app.name} draggable={false} />
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

