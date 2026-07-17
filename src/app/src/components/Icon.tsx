import React from 'react';
import { ModelCapability } from '../modelCapabilities';
import type { Preset } from '../presetStore';
import { presetIconName } from '../presetStore';
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
  | 'x' | 'tools' | 'chat' | 'omni' | 'image' | 'audio' | 'tts' | 'embedding'
  | 'reranking' | 'model' | 'globe' | 'file' | 'code' | 'vision' | 'logs'
  | 'search' | 'search-check' | 'eye' | 'eye-off' | 'plus' | 'edit' | 'download' | 'play' | 'pause' | 'trash' | 'rotate-ccw' | 'chevron-down' | 'chevron-up' | 'chevron-right' | 'plug' | 'box' | 'alert' | 'clock'
  | 'citrus' | 'scale' | 'scan-eye' | 'gem' | 'gauge' | 'timer' | 'pen-line' | 'library'
  | 'hard-drive' | 'sliders-horizontal' | 'flame' | 'wrench' | 'brain' | 'rocket' | 'pin'
  | 'star' | 'hugging-face' | 'cloud' | 'cloud-off' | 'user-round-cog' | 'router'
  | 'speech' | 'book-open' | 'newspaper' | 'github' | 'discord' | 'funnel' | 'info'
  | 'thermometer' | 'crosshair' | 'compass' | 'lightbulb' | 'scan-text' | 'minimize-2'
  | 'panel-top' | 'expand' | 'maximize-2' | 'brain-off' | 'brain-cog' | 'brain-circuit' | 'wrench-off' | 'terminal-square' | 'settings' | 'layers'
  | 'menu';

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

export type CapabilityIconTarget = ModelCapability | 'all' | 'vision' | 'code' | 'transcription' | 'popular' | 'tool' | 'tools' | 'reasoning' | 'mtp';

export function capabilityIconName(capability: CapabilityIconTarget): IconName {
  switch (capability) {
    case 'all': return 'globe';
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
