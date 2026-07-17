import React from 'react';
import {
  ArrowUp,
  AudioLines,
  BookOpen,
  Box,
  Brain,
  BrainCircuit,
  BrainCog,
  Check,
  ChevronDown,
  ChevronRight,
  ChevronUp,
  Citrus,
  CircleOff,
  Clock,
  Cloud,
  CloudOff,
  Code2,
  Compass,
  Copy,
  Cpu,
  Crosshair,
  Download,
  Expand,
  Eye,
  EyeOff,
  File,
  Flame,
  Funnel,
  Gauge,
  Gem,
  Globe,
  HardDrive,
  Image,
  Info,
  Layers3,
  Library,
  Lightbulb,
  List,
  ListFilter,
  Maximize2,
  Menu,
  MessageCircle,
  Mic,
  Minimize2,
  Moon,
  Network,
  Newspaper,
  PanelTop,
  Paperclip,
  Pause,
  PenLine,
  Pencil,
  Pin,
  Play,
  Plug,
  Plus,
  Rocket,
  RotateCcw,
  Router,
  Scale,
  ScanEye,
  ScanText,
  Search,
  SearchCheck,
  Settings,
  SlidersHorizontal,
  Sparkles,
  Speech,
  Square,
  SquareTerminal,
  Star,
  Sun,
  Thermometer,
  Timer,
  Trash2,
  TriangleAlert,
  UserRoundCog,
  Volume2,
  Wrench,
  WrenchOff,
  X,
  type LucideIcon,
} from 'lucide-react';
import type { IconType } from 'react-icons';
import { SiDiscord, SiGithub, SiHuggingface } from 'react-icons/si';
import { ModelCapability } from '../modelCapabilities';
import type { Preset } from '../presetStore';
import { presetIconName } from '../presetStore';

/**
 * Central icon registry.
 *
 * Do not add hand-authored SVG paths here. Product UI icons belong in
 * lucide-react; third-party brand marks belong in react-icons/si.
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

type BrandIconName = 'hugging-face' | 'github' | 'discord';
type LucideIconName = Exclude<IconName, BrandIconName>;

const LUCIDE_ICONS: Record<LucideIconName, LucideIcon> = {
  sun: Sun,
  moon: Moon,
  paperclip: Paperclip,
  mic: Mic,
  send: ArrowUp,
  stop: Square,
  copy: Copy,
  check: Check,
  x: X,
  tools: Wrench,
  chat: MessageCircle,
  omni: Sparkles,
  image: Image,
  audio: AudioLines,
  tts: Volume2,
  embedding: Network,
  reranking: ListFilter,
  model: Cpu,
  globe: Globe,
  file: File,
  code: Code2,
  vision: Eye,
  logs: List,
  search: Search,
  'search-check': SearchCheck,
  eye: Eye,
  'eye-off': EyeOff,
  plus: Plus,
  edit: Pencil,
  download: Download,
  play: Play,
  pause: Pause,
  trash: Trash2,
  'rotate-ccw': RotateCcw,
  'chevron-down': ChevronDown,
  'chevron-up': ChevronUp,
  'chevron-right': ChevronRight,
  plug: Plug,
  box: Box,
  alert: TriangleAlert,
  clock: Clock,
  citrus: Citrus,
  scale: Scale,
  'scan-eye': ScanEye,
  gem: Gem,
  gauge: Gauge,
  timer: Timer,
  'pen-line': PenLine,
  library: Library,
  'hard-drive': HardDrive,
  'sliders-horizontal': SlidersHorizontal,
  flame: Flame,
  wrench: Wrench,
  brain: Brain,
  rocket: Rocket,
  pin: Pin,
  star: Star,
  cloud: Cloud,
  'cloud-off': CloudOff,
  'user-round-cog': UserRoundCog,
  router: Router,
  speech: Speech,
  'book-open': BookOpen,
  newspaper: Newspaper,
  funnel: Funnel,
  info: Info,
  thermometer: Thermometer,
  crosshair: Crosshair,
  compass: Compass,
  lightbulb: Lightbulb,
  'scan-text': ScanText,
  'minimize-2': Minimize2,
  'panel-top': PanelTop,
  expand: Expand,
  'maximize-2': Maximize2,
  // Lucide currently has no BrainOff glyph; CircleOff preserves the disabled
  // meaning without introducing a hand-authored composite SVG.
  'brain-off': CircleOff,
  'brain-cog': BrainCog,
  'brain-circuit': BrainCircuit,
  'wrench-off': WrenchOff,
  'terminal-square': SquareTerminal,
  settings: Settings,
  layers: Layers3,
  menu: Menu,
};

const BRAND_ICONS: Record<BrandIconName, IconType> = {
  'hugging-face': SiHuggingface,
  github: SiGithub,
  discord: SiDiscord,
};

const iconClassName = (className?: string): string =>
  ['app-icon', className].filter(Boolean).join(' ');

function isBrandIcon(name: IconName): name is BrandIconName {
  return name === 'hugging-face' || name === 'github' || name === 'discord';
}

export const Icon: React.FC<IconProps> = ({ name, size = 16, className, title }) => {
  const sharedProps = {
    className: iconClassName(className),
    'data-icon': name,
    'aria-hidden': title ? undefined : true,
    'aria-label': title,
    role: title ? 'img' : undefined,
    focusable: false,
    title,
  } as const;

  if (isBrandIcon(name)) {
    const BrandIcon = BRAND_ICONS[name];
    return (
      <BrandIcon
        {...sharedProps}
        data-icon-library="simple-icons"
        size={size}
      />
    );
  }

  const LucideComponent = LUCIDE_ICONS[name];
  return (
    <LucideComponent
      {...sharedProps}
      data-icon-library="lucide"
      size={size}
      strokeWidth={1.0}
      absoluteStrokeWidth
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
