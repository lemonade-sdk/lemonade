import React, { useEffect, useState } from 'react';

interface AboutModalProps {
  isOpen: boolean;
  onClose: () => void;
}

const AboutModal: React.FC<AboutModalProps> = ({ isOpen, onClose }) => {
  const [version, setVersion] = useState<string>('Loading...');

  useEffect(() => {
    if (isOpen && window.api?.getVersion) {
      setVersion('Loading...');
      
      // Retry logic to handle backend startup delay
      const fetchVersionWithRetry = async (retries = 3, delay = 1000) => {
        for (let i = 0; i < retries; i++) {
          const v = await window.api.getVersion!();
          if (v !== 'Unknown') {
            setVersion(v);
            return;
          }
          if (i < retries - 1) {
            await new Promise(resolve => setTimeout(resolve, delay));
          }
        }
        setVersion('Unknown (Backend not running)');
      };
      
      fetchVersionWithRetry();
    }
  }, [isOpen]);

  if (!isOpen) return null;

  return (
    <div className="settings-overlay" onClick={onClose}>
      <div className="about-modal" onClick={(e) => e.stopPropagation()}>
        <div className="about-header">
          <h2>About Lemonade</h2>
          <button className="settings-close-button" onClick={onClose} title="Close">
            <svg width="14" height="14" viewBox="0 0 14 14">
              <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
            </svg>
          </button>
        </div>
        <div className="about-content">
          <div className="about-item">
            <span className="about-label">Version:</span>
            <span className="about-value">{version}</span>
          </div>
        </div>
      </div>
    </div>
  );
};

export default AboutModal;

