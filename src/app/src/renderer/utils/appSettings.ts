export type NumericSettingKey = 'temperature' | 'topK' | 'topP' | 'repeatPenalty';
export type BooleanSettingKey = 'enableThinking';
export type SettingKey = NumericSettingKey | BooleanSettingKey;

export interface NumericSetting {
  value: number;
  useDefault: boolean;
}

export interface BooleanSetting {
  value: boolean;
  useDefault: boolean;
}

export interface LayoutSettings {
  isChatVisible: boolean;
  isModelManagerVisible: boolean;
  isCenterPanelVisible: boolean;
  isLogsVisible: boolean;
  modelManagerWidth: number;
  chatWidth: number;
  logsHeight: number;
}

export interface AppSettings {
  temperature: NumericSetting;
  topK: NumericSetting;
  topP: NumericSetting;
  repeatPenalty: NumericSetting;
  enableThinking: BooleanSetting;
  layout: LayoutSettings;
}

type BaseSettingValues = Record<NumericSettingKey, number> & {
  enableThinking: boolean;
};

export const BASE_SETTING_VALUES: BaseSettingValues = {
  temperature: 0.7,
  topK: 40,
  topP: 0.9,
  repeatPenalty: 1.1,
  enableThinking: true,
};

export const NUMERIC_SETTING_LIMITS: Record<NumericSettingKey, { min: number; max: number; step: number }> = {
  temperature: { min: 0, max: 2, step: 0.1 },
  topK: { min: 1, max: 100, step: 1 },
  topP: { min: 0, max: 1, step: 0.01 },
  repeatPenalty: { min: 1, max: 2, step: 0.1 },
};

const numericSettingKeys: NumericSettingKey[] = ['temperature', 'topK', 'topP', 'repeatPenalty'];

export const DEFAULT_LAYOUT_SETTINGS: LayoutSettings = {
  isChatVisible: true,
  isModelManagerVisible: true,
  isCenterPanelVisible: true,
  isLogsVisible: false,
  modelManagerWidth: 280,
  chatWidth: 350,
  logsHeight: 200,
};

export const createDefaultSettings = (): AppSettings => ({
  temperature: { value: BASE_SETTING_VALUES.temperature, useDefault: true },
  topK: { value: BASE_SETTING_VALUES.topK, useDefault: true },
  topP: { value: BASE_SETTING_VALUES.topP, useDefault: true },
  repeatPenalty: { value: BASE_SETTING_VALUES.repeatPenalty, useDefault: true },
  enableThinking: { value: BASE_SETTING_VALUES.enableThinking, useDefault: true },
  layout: { ...DEFAULT_LAYOUT_SETTINGS },
});

export const cloneSettings = (settings: AppSettings): AppSettings => ({
  temperature: { ...settings.temperature },
  topK: { ...settings.topK },
  topP: { ...settings.topP },
  repeatPenalty: { ...settings.repeatPenalty },
  enableThinking: { ...settings.enableThinking },
  layout: { ...settings.layout },
});

export const clampNumericSettingValue = (key: NumericSettingKey, value: number): number => {
  const { min, max } = NUMERIC_SETTING_LIMITS[key];

  if (!Number.isFinite(value)) {
    return BASE_SETTING_VALUES[key];
  }

  return Math.min(Math.max(value, min), max);
};

export const mergeWithDefaultSettings = (incoming?: Partial<AppSettings>): AppSettings => {
  const defaults = createDefaultSettings();

  if (!incoming) {
    return defaults;
  }

  numericSettingKeys.forEach((key) => {
    const rawSetting = incoming[key];
    if (!rawSetting || typeof rawSetting !== 'object') {
      return;
    }

    const useDefault =
      typeof rawSetting.useDefault === 'boolean'
        ? rawSetting.useDefault
        : defaults[key].useDefault;
    const numericValue = useDefault
      ? defaults[key].value
      : typeof rawSetting.value === 'number'
        ? clampNumericSettingValue(key, rawSetting.value)
        : defaults[key].value;

    defaults[key] = {
      value: numericValue,
      useDefault,
    };
  });

  const rawEnableThinking = incoming.enableThinking;
  if (rawEnableThinking && typeof rawEnableThinking === 'object') {
    const useDefault =
      typeof rawEnableThinking.useDefault === 'boolean'
        ? rawEnableThinking.useDefault
        : defaults.enableThinking.useDefault;
    const value = useDefault
      ? defaults.enableThinking.value
      : typeof rawEnableThinking.value === 'boolean'
        ? rawEnableThinking.value
        : defaults.enableThinking.value;

    defaults.enableThinking = {
      value,
      useDefault,
    };
  }

  // Merge layout settings
  const rawLayout = incoming.layout;
  if (rawLayout && typeof rawLayout === 'object') {
    // Merge boolean visibility settings
    if (typeof rawLayout.isChatVisible === 'boolean') {
      defaults.layout.isChatVisible = rawLayout.isChatVisible;
    }
    if (typeof rawLayout.isModelManagerVisible === 'boolean') {
      defaults.layout.isModelManagerVisible = rawLayout.isModelManagerVisible;
    }
    if (typeof rawLayout.isCenterPanelVisible === 'boolean') {
      defaults.layout.isCenterPanelVisible = rawLayout.isCenterPanelVisible;
    }
    if (typeof rawLayout.isLogsVisible === 'boolean') {
      defaults.layout.isLogsVisible = rawLayout.isLogsVisible;
    }
    // Merge numeric size settings
    if (typeof rawLayout.modelManagerWidth === 'number') {
      defaults.layout.modelManagerWidth = rawLayout.modelManagerWidth;
    }
    if (typeof rawLayout.chatWidth === 'number') {
      defaults.layout.chatWidth = rawLayout.chatWidth;
    }
    if (typeof rawLayout.logsHeight === 'number') {
      defaults.layout.logsHeight = rawLayout.logsHeight;
    }
  }

  return defaults;
};

export const buildChatRequestOverrides = (settings?: AppSettings | null): Record<string, number | boolean> => {
  if (!settings) {
    return {};
  }

  const overrides: Record<string, number | boolean> = {};

  if (!settings.temperature.useDefault) {
    overrides.temperature = Number(settings.temperature.value.toFixed(4));
  }

  if (!settings.topK.useDefault) {
    overrides.top_k = Math.round(settings.topK.value);
  }

  if (!settings.topP.useDefault) {
    overrides.top_p = Number(settings.topP.value.toFixed(4));
  }

  if (!settings.repeatPenalty.useDefault) {
    overrides.repeat_penalty = Number(settings.repeatPenalty.value.toFixed(4));
  }

  if (!settings.enableThinking.useDefault) {
    overrides.enable_thinking = settings.enableThinking.value;
  }

  return overrides;
};

