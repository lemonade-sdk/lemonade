import type { ModelInfo } from '../../api';
import { capabilityFromModelInfo, isCollectionRecipe, isOnDemandModelInfo } from '../../modelCapabilities';

export type VariantSource = 'huggingface' | 'modelscope' | 'local_path';

export function variantModelId(model: ModelInfo): string {
  return String(model.model_name ?? model.name ?? model.id ?? '').trim();
}

export function variantRecipe(model: ModelInfo): string {
  return String(model.recipe ?? '').trim().toLowerCase();
}

// Only compare explicit, understood artifact origins. An upload or a model in
// an extra models directory may have its own placement/ownership semantics.
export function variantSource(model: ModelInfo): VariantSource | null {
  const source = String(model.source ?? '').trim().toLowerCase();
  const registry = String(model.registry_source ?? '').trim().toLowerCase();
  if (source === 'local_path') return source;
  if (source && source !== 'huggingface' && source !== 'modelscope') return null;
  if (source && registry && source !== registry) return null;
  const resolved = source || registry;
  return resolved === 'huggingface' || resolved === 'modelscope' ? resolved : null;
}

function checkpointRoles(model: ModelInfo): Record<string, string> | null {
  const roles: Record<string, string> = Object.create(null);
  const checkpoints = model.checkpoints;
  if (checkpoints != null) {
    if (typeof checkpoints !== 'object' || Array.isArray(checkpoints)) return null;
    for (const [role, checkpoint] of Object.entries(checkpoints)) {
      if (typeof checkpoint !== 'string' || !checkpoint.trim()) return null;
      roles[role] = checkpoint.trim();
    }
  }
  for (const [role, checkpoint] of [['main', model.checkpoint], ['mmproj', model.mmproj]] as const) {
    if (checkpoint == null || checkpoint === '') continue;
    if (typeof checkpoint !== 'string' || !checkpoint.trim()) return null;
    if (roles[role] && roles[role] !== checkpoint.trim()) return null;
    roles[role] = checkpoint.trim();
  }
  return roles.main ? roles : null;
}

// This is an identity check, NOT a claim that two engines are compatible.
// Quantization, all companion roles, source and capability must agree exactly.
export function engineVariantKey(model: ModelInfo): string | null {
  const recipe = variantRecipe(model);
  if (!variantModelId(model) || !recipe || isCollectionRecipe(recipe) || isOnDemandModelInfo(model)) return null;
  const source = variantSource(model);
  const roles = checkpointRoles(model);
  const capability = capabilityFromModelInfo(model);
  if (!source || !roles || capability === 'unknown') return null;
  return JSON.stringify([source, capability, Object.entries(roles).sort(([a], [b]) => a.localeCompare(b))]);
}

// The existing custom model form only represents these roles. Never silently
// drop unrepresentable artifacts when opening a prefilled form.
export function canAddEngineVariant(model: ModelInfo): boolean {
  if (!engineVariantKey(model)) return false;
  const roles = checkpointRoles(model)!;
  const recipe = variantRecipe(model);
  const capability = capabilityFromModelInfo(model);
  return Object.keys(roles).every(role => role === 'main'
    || (role === 'mmproj' && recipe === 'llamacpp' && capability === 'chat')
    || (['text_encoder', 'vae'].includes(role) && ['sd-cpp', 'thenoise'].includes(recipe) && capability === 'image'));
}

export type EngineVariantGroups = ReadonlyMap<string, readonly ModelInfo[]>;

export function engineVariantGroups(models: readonly ModelInfo[]): EngineVariantGroups {
  const buckets = new Map<string, ModelInfo[]>();
  for (const model of models) {
    const key = engineVariantKey(model);
    if (key === null) continue;
    const bucket = buckets.get(key);
    if (bucket) bucket.push(model);
    else buckets.set(key, [model]);
  }
  const result = new Map<string, readonly ModelInfo[]>();
  for (const bucket of buckets.values()) {
    // Same-engine custom configurations/aliases alone are not variants.
    if (new Set(bucket.map(variantRecipe)).size < 2) continue;
    for (const model of bucket) result.set(variantModelId(model), bucket);
  }
  return result;
}

interface VariantRow {
  model: ModelInfo;
  status: 'running' | 'downloading' | 'downloaded' | 'available';
  pinned?: boolean;
}

// Apply AFTER the normal search and filters. Keep a real matching model as
// representative; pinned/running/downloading rows take priority so an idle
// variant cannot hide activity. The selector still exposes every real ID.
export function collapseEngineVariantRows<T extends VariantRow>(
  rows: readonly T[], groups: EngineVariantGroups, selectedId: string | null,
): T[] {
  const representative = new Map<readonly ModelInfo[], T>();
  const rank = (row: T) => row.pinned ? 0 : row.status === 'running' ? 1
    : row.status === 'downloading' ? 2 : row.status === 'downloaded' ? 3 : 4;
  for (const row of rows) {
    const group = groups.get(variantModelId(row.model));
    if (!group) continue;
    const prev = representative.get(group);
    if (!prev || rank(row) < rank(prev) ||
        (rank(row) === rank(prev) && variantModelId(row.model) === selectedId)) {
      representative.set(group, row);
    }
  }
  // Retain the representative's original position in the already sorted list.
  return rows.filter(row => {
    const group = groups.get(variantModelId(row.model));
    return !group || representative.get(group) === row;
  });
}

export function engineVariantRowSelected(
  model: ModelInfo, selectedId: string | null, groups: EngineVariantGroups,
): boolean {
  if (variantModelId(model) === selectedId) return true;
  return selectedId !== null && (groups.get(variantModelId(model))?.some(m => variantModelId(m) === selectedId) ?? false);
}

// Registration already uses the user.* namespace and accepts replacement.
// Do not intentionally shadow any existing ID when creating a new variant.
export function variantNameTaken(name: string, models: readonly ModelInfo[]): boolean {
  const key = (id: string) => id.trim().toLowerCase().replace(/^(user|builtin|extra)\./, '');
  return models.some(model => key(variantModelId(model)) === key(name));
}
