import React from 'react';
import { ModelIdentity } from '../modelCapabilities';
import { LOCAL_ICON_DEFINITIONS, LocalIcon } from './localIcons';

/**
 * Central icon registry.
 *
 * Icon geometry is vendored locally so npm-based desktop builds and distro
 * system-module builds render the same GUI3 icons without requiring
 * lucide-react or react-icons at package-build time.
 */

export type IconName =
  | 'sun' | 'moon' | 'paperclip' | 'mic' | 'send' | 'stop' | 'copy' | 'check'
  | 'x' | 'eject' | 'tools' | 'chat' | 'omni' | 'image' | 'audio' | 'tts' | 'embedding'
  | 'reranking' | 'model' | 'globe' | 'file' | 'code' | 'vision' | 'logs'
  | 'search' | 'search-check' | 'eye' | 'eye-off' | 'plus' | 'edit' | 'compose' | 'download' | 'file-up' | 'folder' | 'play' | 'pause' | 'trash' | 'rotate-ccw' | 'chevron-down' | 'chevron-up' | 'chevron-right' | 'plug' | 'box' | 'alert' | 'clock'
  | 'sliders-horizontal' | 'flame' | 'wrench' | 'brain' | 'rocket' | 'pin'
  | 'star' | 'hugging-face' | 'cloud' | 'cloud-off' | 'user-round-cog' | 'router'
  | 'speech' | 'book-open' | 'newspaper' | 'github' | 'discord' | 'funnel' | 'info'
  | 'thermometer' | 'gem' | 'gauge' | 'timer' | 'hard-drive' | 'library' | 'scan-eye' | 'minimize-2'
  | 'panel-left-close' | 'panel-left-open' | 'maximize-2' | 'brain-off' | 'brain-cog' | 'brain-circuit' | 'wrench-off' | 'terminal-square' | 'settings' | 'layers'
  | 'menu' | 'flask-conical' | 'external-link'
  | 'model-details';

interface IconProps {
  name: IconName;
  size?: number;
  className?: string;
  title?: string;
}

const iconClassName = (className?: string): string =>
  ['app-icon', className].filter(Boolean).join(' ');

export const Icon: React.FC<IconProps> = ({ name, size = 16, className, title }) => {
  const definition = LOCAL_ICON_DEFINITIONS[name];

  if (!definition) {
    return null;
  }

  return (
    <LocalIcon
      definition={definition}
      size={size}
      className={iconClassName(className)}
      title={title}
      data-icon={name}
      data-icon-library={definition.brand ? 'simple-icons' : 'lucide'}
    />
  );
};

export type CapabilityIconTarget = ModelIdentity | 'all' | 'vision' | 'code' | 'transcription' | 'hot' | 'popular' | 'tool' | 'tools' | 'reasoning' | 'mtp';

export function capabilityIconName(capability: CapabilityIconTarget): IconName {
  switch (capability) {
    case 'all': return 'globe';
    case 'router': return 'router';
    case 'hot': return 'flame';
    case 'popular': return 'flame';
    case 'tool': return 'wrench';
    case 'tools': return 'wrench';
    case 'reasoning': return 'brain';
    case 'mtp': return 'rocket';
    case 'chat': return 'chat';
    case 'omni': return 'omni';
    case 'image': return 'image';
    case 'audio': return 'audio';
    case 'audio-generation': return 'audio';
    case 'transcription': return 'mic';
    case 'tts': return 'tts';
    case 'model3d': return 'box';
    case 'embedding': return 'embedding';
    case 'reranking': return 'reranking';
    case 'classification': return 'search-check';
    case 'vision': return 'vision';
    case 'code': return 'code';
    default: return 'model';
  }
}

export const CapabilityIcon: React.FC<{ capability: CapabilityIconTarget; size?: number; className?: string; title?: string }> = ({ capability, size, className, title }) => (
  <Icon name={capabilityIconName(capability)} size={size} className={className} title={title} />
);
