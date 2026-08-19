import type { ModelInfo } from './modelData';
import type { SystemInfo } from './systemData';

export interface GenerationParamMetadata {
  name: string;
  label: string;
  typeName: string;
  defaultValue: unknown;
  min: number | null;
  max: number | null;
  step: number | null;
  enumValues: string[];
  help: string;
  group: string;
  exclusiveGroup: string;
  accept: string;
  randomSentinel: number | null;
}

const asRecord = (value: unknown): Record<string, unknown> => (
  value && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : {}
);

const asString = (value: unknown): string => (
  typeof value === 'string' ? value.trim() : ''
);

const asNumberOrNull = (value: unknown): number | null => (
  typeof value === 'number' && Number.isFinite(value) ? value : null
);

export const generationParams = (
  systemInfo: SystemInfo | undefined,
  recipe: string,
  mode: string,
): GenerationParamMetadata[] => {
  const recipeInfo = asRecord(systemInfo?.recipes?.[recipe]);
  const byMode = asRecord(recipeInfo.generation_params);
  const raw = byMode[mode];
  if (!Array.isArray(raw)) return [];

  return raw.map((entry) => {
    const param = asRecord(entry);
    return {
      name: asString(param.name),
      label: asString(param.label),
      typeName: asString(param.type_name),
      defaultValue: param.default,
      min: asNumberOrNull(param.min),
      max: asNumberOrNull(param.max),
      step: asNumberOrNull(param.step),
      enumValues: Array.isArray(param.enum_values)
        ? param.enum_values.map(asString).filter(Boolean)
        : [],
      help: asString(param.help),
      group: asString(param.group),
      exclusiveGroup: asString(param.exclusive_group),
      accept: asString(param.accept),
      randomSentinel: asNumberOrNull(param.random_sentinel),
    };
  }).filter(param => param.name && param.typeName);
};

export const generationParamDefault = (
  param: GenerationParamMetadata,
  modelDefaults: object,
): unknown => {
  const modelDefault = asRecord(modelDefaults)[param.name];
  return modelDefault !== undefined && modelDefault !== null
    ? modelDefault
    : param.defaultValue;
};

export const resolveGenerationSeed = (param: GenerationParamMetadata): number => {
  if (param.randomSentinel !== null) return param.randomSentinel;
  const low = param.min ?? 0;
  const high = param.max ?? 0xffffffff;
  return low + Math.floor(Math.random() * Math.max(1, high - low));
};

export type TtsVoiceMode = 'fixed' | 'clone';

export const getTtsVoiceMode = (
  systemInfo: SystemInfo | undefined,
  info?: ModelInfo | null,
): TtsVoiceMode => {
  if (!info) return 'fixed';
  return generationParams(systemInfo, info.recipe, 'tts')
    .some(param => param.typeName === 'AUDIO_B64' && param.exclusiveGroup)
    ? 'clone'
    : 'fixed';
};
