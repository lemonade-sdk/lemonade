import React, { useEffect, useRef, useState } from 'react';

interface AboutModalProps {
  isOpen: boolean;
  onClose: () => void;
}

const AboutModal: React.FC<AboutModalProps> = ({ isOpen, onClose }) => {
  const [version, setVersion] = useState<string>('Loading...');
  const cardRef = useRef<HTMLDivElement>(null);

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

  useEffect(() => {
    if (!isOpen) return;

    const handleClickOutside = (event: MouseEvent) => {
      if (cardRef.current && !cardRef.current.contains(event.target as Node)) {
        onClose();
      }
    };

    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        onClose();
      }
    };

    document.addEventListener('mousedown', handleClickOutside);
    document.addEventListener('keydown', handleKeyDown);

    return () => {
      document.removeEventListener('mousedown', handleClickOutside);
      document.removeEventListener('keydown', handleKeyDown);
    };
  }, [isOpen, onClose]);

  if (!isOpen) return null;

  return (
    <div className="about-popover" ref={cardRef}>
      <div className="about-popover-header">
        <div>
          <p className="about-popover-title">Lemonade</p>
          <p className="about-popover-subtitle">Local AI control center</p>
        </div>
        <button className="about-popover-close" onClick={onClose} title="Close">
          <svg width="14" height="14" viewBox="0 0 14 14">
            <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
          </svg>
        </button>
      </div>

      <div className="about-popover-body">
        <div className="about-popover-version">
          <span>Version</span>
          <span>{version}</span>
        </div>
      </div>
    </div>
  );
};

export default AboutModal;

