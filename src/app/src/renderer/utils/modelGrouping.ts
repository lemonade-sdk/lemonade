import { ModelInfo } from './modelData';

export interface ModelFamily {
  displayName: string;
  regex: RegExp;
}

const SIZE_TOKEN = String.raw`(\d+\.?\d*B(?:-A\d+\.?\d*B)?)`;
const FLM_SIZE_TOKEN = String.raw`(\d+\.?\d*[bm])`;

function buildFamilyRegex(prefix: string, suffix = '-GGUF$'): RegExp {
  return new RegExp(`^${prefix}-${SIZE_TOKEN}${suffix}`);
}

function buildFlmFamilyRegex(prefix: string): RegExp {
  return new RegExp(`^${prefix}-${FLM_SIZE_TOKEN}-FLM$`);
}

export const MODEL_FAMILIES: ModelFamily[] = [
  // Standardized family matching: capture *B or *B-A*B.
  {
    displayName: 'Qwen3',
    regex: buildFamilyRegex('Qwen3'),
  },
  {
    displayName: 'Qwen3-Instruct-2507',
    regex: buildFamilyRegex('Qwen3', '-Instruct-2507-GGUF$'),
  },
  {
    displayName: 'Qwen3.5',
    regex: buildFamilyRegex('Qwen3\\.5'),
  },
  {
    displayName: 'Qwen3-Embedding',
    regex: buildFamilyRegex('Qwen3-Embedding'),
  },
  {
    displayName: 'Qwen2.5-VL-Instruct',
    regex: buildFamilyRegex('Qwen2\\.5-VL', '-Instruct-GGUF$'),
  },
  {
    displayName: 'Qwen3-VL-Instruct',
    regex: buildFamilyRegex('Qwen3-VL', '-Instruct-GGUF$'),
  },
  {
    displayName: 'Llama-3.2-Instruct',
    regex: buildFamilyRegex('Llama-3\\.2', '-Instruct-GGUF$'),
  },
  {
    displayName: 'gpt-oss',
    regex: /^gpt-oss-(\d+\.?\d*b)-mxfp4?-GGUF$/,
  },
  {
    displayName: 'LFM2',
    regex: buildFamilyRegex('LFM2'),
  },
  // FLM families
  {
    displayName: 'gemma3',
    regex: buildFlmFamilyRegex('gemma3'),
  },
  {
    displayName: 'lfm2',
    regex: buildFlmFamilyRegex('lfm2'),
  },
  {
    displayName: 'llama3.2',
    regex: buildFlmFamilyRegex('llama3\\.2'),
  },
  {
    displayName: 'qwen3',
    regex: buildFlmFamilyRegex('qwen3'),
  },
];

export type ModelListItem =
  | { type: 'model'; name: string; info: ModelInfo }
  | { type: 'family'; family: ModelFamily; members: { label: string; name: string; info: ModelInfo }[] }
  | {
      type: 'dynamic-group';
      groupName: string;
      defaultExpanded: boolean;
      members: { label: string; name: string; info: ModelInfo }[];
    };

/**
 * Builds a structured model list from a flat array of models,
 * grouping them into families and dynamic groups based on naming patterns.
 */
export function buildModelList(
  models: Array<{ name: string; info: ModelInfo }>
): ModelListItem[] {
  // Build family groups
  const consumed = new Set<string>();
  const familyItems: ModelListItem[] = [];

  for (const family of MODEL_FAMILIES) {
    const members: { label: string; name: string; info: ModelInfo }[] = [];
    for (const m of models) {
      const match = family.regex.exec(m.name);
      if (match) {
        members.push({ label: match[1], name: m.name, info: m.info });
        consumed.add(m.name);
      }
    }
    if (members.length > 1) {
      // Sort members (usually by size token like 8B)
      members.sort((a, b) => {
        const floatA = parseFloat(a.label);
        const floatB = parseFloat(b.label);
        if (!isNaN(floatA) && !isNaN(floatB)) {
          return floatA - floatB;
        }
        return a.label.localeCompare(b.label);
      });
      familyItems.push({ type: 'family', family, members });
    } else {
      members.forEach(m => consumed.delete(m.name));
    }
  }

  const remainingModels = models.filter(m => !consumed.has(m.name));
  const dynamicCandidates = new Map<string, { label: string; name: string; info: ModelInfo }[]>();

  for (const model of remainingModels) {
    const segments = model.name.split('.');
    if (segments.length < 2) continue;

    // For 3+ segments, group by the first two (e.g., user.provider).
    // For 2 segments, group by the first segment (e.g., provider).
    const groupName = segments.length >= 3 ? segments.slice(0, 2).join('.') : segments[0];
    const label = segments.length >= 3 ? segments.slice(2).join('.') : segments[1];

    if (!dynamicCandidates.has(groupName)) {
      dynamicCandidates.set(groupName, []);
    }
    dynamicCandidates.get(groupName)!.push({ label, name: model.name, info: model.info });
  }

  const dynamicallyGrouped = new Set<string>();
  const dynamicGroupItems: ModelListItem[] = [];
  for (const [groupName, members] of dynamicCandidates) {
    if (members.length < 2) continue;
    members.sort((a, b) => a.label.localeCompare(b.label));
    members.forEach(member => dynamicallyGrouped.add(member.name));
    dynamicGroupItems.push({
      type: 'dynamic-group',
      groupName,
      defaultExpanded: groupName.startsWith('user.'),
      members,
    });
  }

  // Build individual items for non-consumed and non-dynamically-grouped models
  const individualItems: ModelListItem[] = remainingModels
    .filter(m => !dynamicallyGrouped.has(m.name))
    .map(m => ({ type: 'model' as const, name: m.name, info: m.info }));

  // Helper for sorting display names
  const getItemName = (item: ModelListItem) => {
    switch (item.type) {
      case 'family': return item.family.displayName;
      case 'dynamic-group': return item.groupName;
      default: return item.name;
    }
  };

  // Merge and sort alphabetically by display name
  const allItems = [...familyItems, ...dynamicGroupItems, ...individualItems];
  allItems.sort((a, b) => getItemName(a).localeCompare(getItemName(b)));

  return allItems;
}
