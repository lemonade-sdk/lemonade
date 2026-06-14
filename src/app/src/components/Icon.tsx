import React from 'react';
import { ModelCapability } from '../modelCapabilities';
import type { Preset } from '../presetStore';
import { presetIconName } from '../presetStore';

export type IconName =
  | 'sun' | 'moon' | 'paperclip' | 'mic' | 'send' | 'stop' | 'copy' | 'check'
  | 'x' | 'tools' | 'chat' | 'omni' | 'image' | 'audio' | 'tts' | 'embedding'
  | 'reranking' | 'model' | 'globe' | 'file' | 'code' | 'vision' | 'logs'
  | 'search' | 'edit' | 'download' | 'play' | 'plug' | 'box' | 'alert' | 'clock'
  | 'citrus' | 'scale' | 'gem' | 'gauge' | 'timer' | 'pen-line' | 'library'
  | 'hard-drive' | 'sliders-horizontal' | 'flame' | 'wrench' | 'brain' | 'rocket';

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
      case 'play': return <path d="M8 5.5v13l11-6.5-11-6.5z" />;
      case 'plug': return <><path d="M8 2v5M16 2v5" /><path d="M7 7h10v4a5 5 0 01-10 0V7z" /><path d="M12 16v6" /></>;
      case 'box': return <><path d="M21 16V8l-9-5-9 5v8l9 5 9-5z" /><path d="M3.5 8.5L12 13l8.5-4.5" /><path d="M12 13v8" /></>;
      case 'alert': return <><path d="M12 9v4" /><path d="M12 17h.01" /><path d="M10.3 3.9L2.8 17a2 2 0 001.7 3h15a2 2 0 001.7-3L13.7 3.9a2 2 0 00-3.4 0z" /></>;
      case 'clock': return <><circle cx="12" cy="12" r="9" /><path d="M12 7v5l3 2" /></>;
      case 'citrus': return <><path d="M6.5 17.5a7.8 7.8 0 010-11 7.8 7.8 0 0111 11 7.8 7.8 0 01-11 0z" /><path d="M12 4.5V19" /><path d="M5 12h14" /><path d="M8.2 7.8l7.9 7.9" /><path d="M16.1 7.8l-7.9 7.9" /><path d="M14.7 3.2c1.3-.6 2.7-.6 4.1.1-.7 1.3-1.8 2.2-3.1 2.6" /></>;
      case 'scale': return <><path d="M12 3v18" /><path d="M6 7h12" /><path d="M6 7l-4 7h8L6 7z" /><path d="M18 7l-4 7h8l-4-7z" /><path d="M7 21h10" /></>;
      case 'gem': return <><path d="M6 3h12l4 6-10 12L2 9l4-6z" /><path d="M2 9h20" /><path d="M8 3l4 18 4-18" /></>;
      case 'gauge': return <><path d="M4 15a8 8 0 1116 0" /><path d="M12 15l4-5" /><path d="M8 19h8" /></>;
      case 'timer': return <><path d="M10 2h4" /><path d="M12 14l3-3" /><circle cx="12" cy="13" r="8" /></>;
      case 'pen-line': return <><path d="M4 20h4l11-11a2.1 2.1 0 00-3-3L5 17v3z" /><path d="M14 7l3 3" /><path d="M3 22h18" /></>;
      case 'library': return <><path d="M4 19.5V5a2 2 0 012-2h12v16H6a2 2 0 00-2 2" /><path d="M8 7h6" /><path d="M8 11h6" /><path d="M8 15h4" /></>;
      case 'hard-drive': return <><path d="M4 12l2-7h12l2 7" /><rect x="4" y="12" width="16" height="8" rx="2" /><path d="M7 16h.01" /><path d="M17 16h.01" /></>;
      case 'sliders-horizontal': return <><path d="M4 6h7" /><path d="M15 6h5" /><circle cx="13" cy="6" r="2" /><path d="M4 12h3" /><path d="M11 12h9" /><circle cx="9" cy="12" r="2" /><path d="M4 18h10" /><path d="M18 18h2" /><circle cx="16" cy="18" r="2" /></>;
      case 'flame': return <><path d="M8.5 14.5A4 4 0 0012 21a5.5 5.5 0 005.5-5.5c0-3.5-2.5-5.2-3.1-8.5-1.5 1.1-2.2 2.4-2.4 4.2C10.6 9.8 10.2 7.4 10.5 5 8.2 6.6 6.5 9.2 6.5 12.5c0 .8.3 1.5.7 2.1" /><path d="M12 21a2.7 2.7 0 002.7-2.7c0-1.4-.8-2.3-1.7-3.3-.2 1-.7 1.7-1.3 2.2-.6-.8-.8-1.7-.7-2.6-1 .8-1.7 2-1.7 3.3A2.7 2.7 0 0012 21z" /></>;
      case 'wrench': return <><path d="M14.7 6.3a4 4 0 01-5 5L4.5 16.5a2.1 2.1 0 103 3l5.2-5.2a4 4 0 005-5l-2.5 2.5-3-3 2.5-2.5z" /></>;
      case 'brain': return <><path d="M9 4a3 3 0 00-3 3v.4A3.5 3.5 0 003.5 11 3.5 3.5 0 006 14.4V17a3 3 0 003 3" /><path d="M15 4a3 3 0 013 3v.4a3.5 3.5 0 012.5 3.6 3.5 3.5 0 01-2.5 3.4V17a3 3 0 01-3 3" /><path d="M9 4v16M15 4v16M9 8h2M13 8h2M9 12h2M13 12h2M9 16h2M13 16h2" /></>;
      case 'rocket': return <><path d="M5 19c1.5-.4 2.8-1.2 3.8-2.2" /><path d="M15 14l-5-5c1.8-3.2 4.8-5.3 9-6 0 4.2-2 7.2-5.2 9" /><path d="M9 15l-3 3" /><path d="M14 9h.01" /><path d="M7 11l-3 1 2-4 4-2" /><path d="M13 17l-1 3 4-2 2-4" /></>;
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

export type CapabilityIconTarget = ModelCapability | 'all' | 'vision' | 'code' | 'transcription' | 'popular' | 'tools' | 'reasoning' | 'mtp';

export function capabilityIconName(capability: CapabilityIconTarget): IconName {
  switch (capability) {
    case 'all': return 'globe';
    case 'popular': return 'flame';
    case 'tools': return 'wrench';
    case 'reasoning': return 'brain';
    case 'mtp': return 'rocket';
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

export const CapabilityIcon: React.FC<{ capability: CapabilityIconTarget; size?: number; className?: string; title?: string }> = ({ capability, size, className, title }) => (
  <Icon name={capabilityIconName(capability)} size={size} className={className} title={title} />
);


export const PresetIcon: React.FC<{ preset: Pick<Preset, 'id' | 'name' | 'starter'> | null | undefined; size?: number; className?: string; title?: string }> = ({ preset, size = 14, className, title }) => (
  <Icon name={presetIconName(preset)} size={size} className={className} title={title || preset?.name} />
);
