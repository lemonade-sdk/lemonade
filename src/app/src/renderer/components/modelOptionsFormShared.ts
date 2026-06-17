export const BACKEND_DISPLAY_NAMES: Record<string, string> = {
  cpu: "CPU",
  npu: "NPU",
  rocm: "ROCm",
  vulkan: "Vulkan",
  metal: "Metal",
};

export const getBackendDisplayName = (backend: string): string => {
  return BACKEND_DISPLAY_NAMES[backend] ?? backend;
};

export const CONTEXT_SLIDER_MIN = 2048;
export const CONTEXT_SLIDER_THUMB_SIZE = 14;

export const formatContextSize = (value: number): string => {
  if (value >= 1024 && value % 1024 === 0) {
    return `${value / 1024}k`;
  }
  if (value >= 1024) {
    return `${Math.round(value / 1024)}k`;
  }
  return String(value);
};

export const getContextSliderMarks = (maxContextWindow?: number): number[] => {
  if (!maxContextWindow || maxContextWindow < CONTEXT_SLIDER_MIN) {
    return [];
  }

  const marks: number[] = [];
  for (let value = CONTEXT_SLIDER_MIN; value < maxContextWindow; value *= 2) {
    marks.push(value);
  }

  if (marks[marks.length - 1] !== maxContextWindow) {
    marks.push(maxContextWindow);
  }

  return marks;
};

export const contextSizeToSliderValue = (contextSize: number, maxContextWindow: number): number => {
  const clamped = contextSize === 0
    ? maxContextWindow
    : Math.min(Math.max(contextSize, CONTEXT_SLIDER_MIN), maxContextWindow);
  return Math.log2(clamped);
};

export const sliderValueToContextSize = (sliderValue: number, maxContextWindow: number): number => {
  const maxSliderValue = Math.log2(maxContextWindow);
  if (sliderValue >= maxSliderValue - 0.0005) {
    return maxContextWindow;
  }
  return Math.min(Math.round(2 ** sliderValue), maxContextWindow);
};

export const parseAliasInput = (raw: string): string[] => {
  const seen = new Set<string>();
  const result: string[] = [];
  for (const part of raw.split(',')) {
    const alias = part.trim();
    if (!alias || seen.has(alias)) continue;
    seen.add(alias);
    result.push(alias);
  }
  return result;
};

export const formatAliasInput = (aliases: string[]): string => aliases.join(', ');
