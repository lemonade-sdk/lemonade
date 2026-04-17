import { ModelInfo } from './modelData';

export interface ModelFamily {
  displayName: string;
  regex: RegExp;
  userRegex?: RegExp;
}

const SIZE_TOKEN = String.raw`(\d+\.?\d*B(?:-A\d+\.?\d*B)?)`;
const FLM_SIZE_TOKEN = String.raw`(\d+\.?\d*[bm])`;

function buildFamilyRegex(prefix: string, suffix = '-GGUF$'): RegExp {
  return new RegExp(`^${prefix}-${SIZE_TOKEN}${suffix}`);
}

function buildUserFamilyRegex(prefix: string): RegExp {
  return new RegExp(`^${prefix}-${SIZE_TOKEN}(?:-.+)?$`);
}

function buildFlmFamilyRegex(prefix: string): RegExp {
  return new RegExp(`^${prefix}-${FLM_SIZE_TOKEN}-FLM$`);
}

function getGroupingName(modelName: string): string {
  return modelName.startsWith('user.') ? modelName.slice('user.'.length) : modelName;
}

function buildDisplayNameResolver(models: Array<{ name: string; info: ModelInfo }>) {
  const visibleNameCounts = new Map<string, number>();
  for (const model of models) {
    const visibleName = getGroupingName(model.name);
    visibleNameCounts.set(visibleName, (visibleNameCounts.get(visibleName) || 0) + 1);
  }

  return (modelName: string) => {
    const visibleName = getGroupingName(modelName);
    if (modelName.startsWith('user.') && (visibleNameCounts.get(visibleName) || 0) > 1) {
      return modelName;
    }
    return visibleName;
  };
}

export const MODEL_FAMILIES: ModelFamily[] = [
  // Standardized family matching: capture *B or *B-A*B.
  {
    displayName: 'Qwen3',
    regex: buildFamilyRegex('Qwen3'),
    userRegex: buildUserFamilyRegex('Qwen3'),
  },
  {
    displayName: 'Qwen3-Instruct-2507',
    regex: buildFamilyRegex('Qwen3', '-Instruct-2507-GGUF$'),
    userRegex: buildUserFamilyRegex('Qwen3'),
  },
  {
    displayName: 'Qwen3.5',
    regex: buildFamilyRegex('Qwen3\\.5'),
    userRegex: buildUserFamilyRegex('Qwen3\\.5'),
  },
  {
    displayName: 'Qwen3-Embedding',
    regex: buildFamilyRegex('Qwen3-Embedding'),
    userRegex: buildUserFamilyRegex('Qwen3-Embedding'),
  },
  {
    displayName: 'Qwen2.5-VL-Instruct',
    regex: buildFamilyRegex('Qwen2\\.5-VL', '-Instruct-GGUF$'),
    userRegex: buildUserFamilyRegex('Qwen2\\.5-VL'),
  },
  {
    displayName: 'Qwen3-VL-Instruct',
    regex: buildFamilyRegex('Qwen3-VL', '-Instruct-GGUF$'),
    userRegex: buildUserFamilyRegex('Qwen3-VL'),
  },
  {
    displayName: 'Llama-3.2-Instruct',
    regex: buildFamilyRegex('Llama-3\\.2', '-Instruct-GGUF$'),
    userRegex: buildUserFamilyRegex('Llama-3\\.2'),
  },
  {
    displayName: 'gpt-oss',
    regex: /^gpt-oss-(\d+\.?\d*b)-mxfp4?-GGUF$/,
  },
  {
    displayName: 'LFM2',
    regex: buildFamilyRegex('LFM2'),
    userRegex: buildUserFamilyRegex('LFM2'),
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
  | { type: 'model'; name: string; info: ModelInfo; displayName?: string }
  | { type: 'family'; family: ModelFamily; members: { label: string; name: string; info: ModelInfo; displayName?: string }[] }
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
  const getDisplayName = buildDisplayNameResolver(models);

  // Build family groups
  const consumed = new Set<string>();
  const familyItems: ModelListItem[] = [];

  for (const family of MODEL_FAMILIES) {
    const members: { label: string; name: string; info: ModelInfo; displayName?: string }[] = [];
    for (const m of models) {
      const groupingName = getGroupingName(m.name);
      const match = family.regex.exec(groupingName)
        || (m.name.startsWith('user.') && family.userRegex?.exec(groupingName));
      if (match) {
        members.push({ label: match[1], name: m.name, info: m.info });
        consumed.add(m.name);
      }
    }
    if (members.length > 1) {
      const labelCounts = new Map<string, number>();
      for (const member of members) {
        labelCounts.set(member.label, (labelCounts.get(member.label) || 0) + 1);
      }
      for (const member of members) {
        if ((labelCounts.get(member.label) || 0) > 1) {
          member.displayName = getDisplayName(member.name);
        }
      }

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

  // Build individual items for non-consumed models. User-managed models use
  // their display name without the management prefix, but keep the real name for
  // load/download/delete operations.
  const individualItems: ModelListItem[] = remainingModels
    .map(m => ({
      type: 'model' as const,
      name: m.name,
      info: m.info,
      displayName: getDisplayName(m.name),
    }));

  // Helper for sorting display names
  const getItemName = (item: ModelListItem) => {
    switch (item.type) {
      case 'family': return item.family.displayName;
      case 'dynamic-group': return item.groupName;
      default: return item.displayName || item.name;
    }
  };

  // Merge and sort alphabetically by display name
  const allItems = [...familyItems, ...individualItems];
  allItems.sort((a, b) => getItemName(a).localeCompare(getItemName(b)));

  return allItems;
}
