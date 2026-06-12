import React from 'react';
import { ModelCapability } from '../modelCapabilities';

type IconName =
  | 'sun' | 'moon' | 'paperclip' | 'mic' | 'send' | 'stop' | 'copy' | 'check'
  | 'x' | 'tools' | 'chat' | 'omni' | 'image' | 'audio' | 'tts' | 'embedding'
  | 'reranking' | 'model' | 'globe' | 'file' | 'code' | 'vision' | 'logs'
  | 'search' | 'edit' | 'download' | 'plug' | 'box' | 'alert' | 'clock';

interface IconProps {
  name: IconName;
  size?: number;
  className?: string;
  title?: string;
}

const common = {
  fill: 'none',
  stroke: 'currentColor',
  strokeWidth: 1.8,
  strokeLinecap: 'round' as const,
  strokeLinejoin: 'round' as const,
};

export const Icon: React.FC<IconProps> = ({ name, size = 16, className, title }) => {
  const content = (() => {
    switch (name) {
      case 'sun': return <><circle cx="12" cy="12" r="4" /><path d="M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M4.93 19.07l1.41-1.41M17.66 6.34l1.41-1.41" /></>;
      case 'moon': return <path d="M21 12.7A8.5 8.5 0 1111.3 3a6.8 6.8 0 009.7 9.7z" />;
      case 'paperclip': return <path d="M21.44 11.05l-9.19 9.19a6 6 0 01-8.49-8.49l9.19-9.19a4 4 0 015.66 5.66l-9.2 9.19a2 2 0 01-2.83-2.83l8.49-8.48" />;
      case 'mic': return <><rect x="9" y="2.5" width="6" height="11" rx="3" /><path d="M5 10a7 7 0 0014 0M12 17v4M8.5 21h7" /></>;
      case 'send': return <><path d="M12 19V5" /><path d="M5 12l7-7 7 7" /></>;
      case 'stop': return <rect x="6" y="6" width="12" height="12" rx="2" />;
      case 'copy': return <><rect x="9" y="9" width="10" height="10" rx="2" /><path d="M5 15V7a2 2 0 012-2h8" /></>;
      case 'check': return <path d="M20 6L9 17l-5-5" />;
      case 'x': return <><path d="M18 6L6 18" /><path d="M6 6l12 12" /></>;
      case 'tools': return <><path d="M14.7 6.3a4 4 0 01-5 5L4.5 16.5a2.1 2.1 0 103 3l5.2-5.2a4 4 0 005-5l-2.5 2.5-3-3 2.5-2.5z" /><path d="M4 4l5 5" /></>;
      case 'chat': return <><path d="M21 12a8 8 0 01-8 8H7l-4 3v-6.2A8 8 0 1113 20" /></>;
      case 'omni': return <><path d="M12 3l2.4 5.1L20 10.5l-5.6 2.3L12 18l-2.4-5.2L4 10.5l5.6-2.4L12 3z" /><path d="M19 3v4M17 5h4" /></>;
      case 'image': return <><rect x="3" y="5" width="18" height="14" rx="2" /><circle cx="8" cy="10" r="1.5" /><path d="M21 16l-5-5-4 4-2-2-5 5" /></>;
      case 'audio': return <><path d="M4 14v-4" /><path d="M8 18V6" /><path d="M12 21V3" /><path d="M16 18V6" /><path d="M20 14v-4" /></>;
      case 'tts': return <><path d="M4 10v4h4l5 4V6L8 10H4z" /><path d="M16 9a4 4 0 010 6M18.5 6.5a8 8 0 010 11" /></>;
      case 'embedding': return <><circle cx="6" cy="6" r="2" /><circle cx="18" cy="6" r="2" /><circle cx="12" cy="18" r="2" /><path d="M8 7l3 9M16 7l-3 9M8 6h8" /></>;
      case 'reranking': return <><path d="M6 7h12" /><path d="M6 12h9" /><path d="M6 17h5" /><path d="M4 7h.01M4 12h.01M4 17h.01" /></>;
      case 'globe': return <><circle cx="12" cy="12" r="9" /><path d="M3 12h18M12 3a14 14 0 010 18M12 3a14 14 0 000 18" /></>;
      case 'file': return <><path d="M14 3H7a2 2 0 00-2 2v14a2 2 0 002 2h10a2 2 0 002-2V8z" /><path d="M14 3v5h5" /></>;
      case 'code': return <><path d="M8 9l-4 3 4 3" /><path d="M16 9l4 3-4 3" /><path d="M14 5l-4 14" /></>;
      case 'vision': return <><path d="M2 12s3.5-6 10-6 10 6 10 6-3.5 6-10 6S2 12 2 12z" /><circle cx="12" cy="12" r="3" /></>;
      case 'logs': return <><path d="M5 5h14M5 12h14M5 19h14" /><path d="M3 5h.01M3 12h.01M3 19h.01" /></>;
      case 'search': return <><circle cx="11" cy="11" r="7" /><path d="M20 20l-4-4" /></>;
      case 'edit': return <><path d="M4 20h4l10.5-10.5a2.1 2.1 0 00-3-3L5 17v3z" /><path d="M14 7l3 3" /></>;
      case 'download': return <><path d="M12 3v12" /><path d="M7 10l5 5 5-5" /><path d="M5 21h14" /></>;
      case 'plug': return <><path d="M8 2v5M16 2v5" /><path d="M7 7h10v4a5 5 0 01-10 0V7z" /><path d="M12 16v6" /></>;
      case 'box': return <><path d="M21 16V8l-9-5-9 5v8l9 5 9-5z" /><path d="M3.5 8.5L12 13l8.5-4.5" /><path d="M12 13v8" /></>;
      case 'alert': return <><path d="M12 9v4" /><path d="M12 17h.01" /><path d="M10.3 3.9L2.8 17a2 2 0 001.7 3h15a2 2 0 001.7-3L13.7 3.9a2 2 0 00-3.4 0z" /></>;
      case 'clock': return <><circle cx="12" cy="12" r="9" /><path d="M12 7v5l3 2" /></>;
      default: return <><rect x="5" y="5" width="14" height="14" rx="3" /><path d="M9 9h6v6H9z" /></>;
    }
  })();

  return (
    <svg className={className} width={size} height={size} viewBox="0 0 24 24" aria-hidden={title ? undefined : true} role={title ? 'img' : undefined} {...common}>
      {title && <title>{title}</title>}
      {content}
    </svg>
  );
};

export function capabilityIconName(capability: ModelCapability | 'all' | 'vision' | 'code' | 'transcription'): IconName {
  switch (capability) {
    case 'all': return 'globe';
    case 'chat': return 'chat';
    case 'omni': return 'omni';
    case 'image': return 'image';
    case 'audio': return 'audio';
    case 'transcription': return 'mic';
    case 'tts': return 'tts';
    case 'embedding': return 'embedding';
    case 'reranking': return 'reranking';
    case 'vision': return 'vision';
    case 'code': return 'code';
    default: return 'model';
  }
}

export const CapabilityIcon: React.FC<{ capability: ModelCapability | 'all' | 'vision' | 'code' | 'transcription'; size?: number; className?: string; title?: string }> = ({ capability, size, className, title }) => (
  <Icon name={capabilityIconName(capability)} size={size} className={className} title={title} />
);
