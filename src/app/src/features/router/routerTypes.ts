import type { ModelInfo } from '../../api';

export const ROUTER_RECIPE = 'collection.router' as const;
export const ROUTER_SCHEMA_VERSION = '1' as const;
export const SAFE_ROUTER_ID = /^[A-Za-z0-9._-]+$/;
export const MAX_ROUTER_TREE_DEPTH = 64;

export type RouterClassifierType = 'classifier' | 'semantic_similarity' | 'llm';
export type RouterOnError = 'match_true' | 'match_false';
export type RouterRoutingMode = 'rules' | 'llm';
export type RouterGroupOperator = 'all' | 'any' | 'not';
export type RouterLeafType =
  | 'keywords_any'
  | 'keywords_all'
  | 'regex'
  | 'min_chars'
  | 'max_chars'
  | 'has_tools'
  | 'has_images'
  | 'classifier'
  | 'metadata';
export type RouterMetadataComparator = 'equals' | 'any' | 'exists';

export interface RouterClassifier {
  id: string;
  type: RouterClassifierType;
  model: string;
  prompt: string;
  labels: string[];
  defaultLabel?: string;
  referencePhrases: Record<string, string[]>;
  onError: RouterOnError;
}

export interface RouterLlmRouter {
  model: string;
  prompt: string;
}

export interface RouterLeafNode {
  id: string;
  kind: 'leaf';
  type: RouterLeafType;
  textValue?: string;
  numberValue?: number;
  booleanValue?: boolean;
  classifierId?: string;
  label?: string;
  minScore?: number;
  maxScore?: number;
  metadataKey?: string;
  metadataComparator?: RouterMetadataComparator;
  metadataValues?: string;
}

export interface RouterGroupNode {
  id: string;
  kind: 'group';
  operator: RouterGroupOperator;
  children: RouterNode[];
}

export type RouterNode = RouterLeafNode | RouterGroupNode;

export interface RouterRule {
  id: string;
  routeTo: string;
  condition: RouterNode;
  outputsText?: string;
}

export interface RouterDraft {
  modelName?: string;
  name: string;
  candidates: string[];
  defaultModel: string;
  mode: RouterRoutingMode;
  llmRouter: RouterLlmRouter;
  classifiers: RouterClassifier[];
  rules: RouterRule[];
}

export interface RouterPullRequest {
  version: typeof ROUTER_SCHEMA_VERSION;
  model_name: string;
  recipe: typeof ROUTER_RECIPE;
  components: string[];
  routing: {
    candidates: string[];
    default_model: string;
    router?: {
      type: 'llm';
      model: string;
      prompt: string;
    };
    classifiers?: Array<Record<string, unknown>>;
    rules?: Array<Record<string, unknown>>;
  };
}

let generatedId = 0;
export function createRouterNodeId(prefix = 'node'): string {
  generatedId += 1;
  return `${prefix}-${Date.now().toString(36)}-${generatedId.toString(36)}`;
}

export function normalizeRouterModelName(value: string): string {
  const raw = String(value || '').trim().replace(/^user\./i, '');
  const slug = raw
    .replace(/[^A-Za-z0-9._-]+/g, '-')
    .replace(/-+/g, '-')
    .replace(/^[-.]+|[-.]+$/g, '')
    .slice(0, 72);
  return `user.${slug || 'router'}`;
}

export function routerDisplayName(modelName: string): string {
  return String(modelName || '').replace(/^user\./i, '');
}

export function createRouterLeaf(type: RouterLeafType = 'keywords_any'): RouterLeafNode {
  const base: RouterLeafNode = { id: createRouterNodeId('leaf'), kind: 'leaf', type };
  switch (type) {
    case 'keywords_any':
    case 'keywords_all':
    case 'regex':
      base.textValue = '';
      break;
    case 'min_chars':
      base.numberValue = 500;
      break;
    case 'max_chars':
      base.numberValue = 2000;
      break;
    case 'has_tools':
    case 'has_images':
      base.booleanValue = true;
      break;
    case 'classifier':
      base.classifierId = '';
      base.minScore = 0.5;
      break;
    case 'metadata':
      base.metadataKey = '';
      base.metadataComparator = 'equals';
      base.metadataValues = '';
      break;
  }
  return base;
}

export function createRouterGroup(operator: RouterGroupOperator = 'all'): RouterGroupNode {
  return {
    id: createRouterNodeId('group'),
    kind: 'group',
    operator,
    children: operator === 'not'
      ? [createRouterLeaf()]
      : [createRouterLeaf(), createRouterLeaf('has_tools')],
  };
}

export function createRouterRule(index = 0, routeTo = ''): RouterRule {
  return {
    id: `rule-${index + 1}`,
    routeTo,
    condition: createRouterLeaf(),
    outputsText: '',
  };
}

export function createRouterClassifier(index = 0, type: RouterClassifierType = 'classifier'): RouterClassifier {
  return {
    id: `classifier-${index + 1}`,
    type,
    model: '',
    prompt: '',
    labels: type === 'semantic_similarity' ? [] : type === 'llm' ? ['match', 'other'] : ['match'],
    defaultLabel: type === 'semantic_similarity' ? undefined : type === 'llm' ? 'other' : 'match',
    referencePhrases: type === 'semantic_similarity' ? { concept: ['example phrase'] } : {},
    onError: 'match_false',
  };
}

export function createEmptyRouterDraft(): RouterDraft {
  return {
    name: '',
    candidates: [],
    defaultModel: '',
    mode: 'rules',
    llmRouter: { model: '', prompt: '' },
    classifiers: [],
    rules: [createRouterRule(0)],
  };
}

function splitList(value: unknown): string[] {
  if (Array.isArray(value)) return value.map(item => String(item).trim()).filter(Boolean);
  return String(value ?? '').split(',').map(item => item.trim()).filter(Boolean);
}

function routerNodeHasMeaningfulProgress(node: RouterNode): boolean {
  if (node.kind === 'group') return true;
  if (node.type !== 'keywords_any') return true;
  return splitList(node.textValue).length > 0;
}

export function routerDraftHasRulesProgress(draft: RouterDraft): boolean {
  if (draft.classifiers.length > 0) return true;
  if (draft.rules.length === 0) return false;
  if (draft.rules.length > 1) return true;

  const rule = draft.rules[0];
  return rule.id !== 'rule-1'
    || rule.routeTo !== draft.defaultModel
    || Boolean(rule.outputsText?.trim())
    || routerNodeHasMeaningfulProgress(rule.condition);
}

export function routerDraftHasLlmProgress(draft: RouterDraft): boolean {
  return Boolean(draft.llmRouter.model.trim() || draft.llmRouter.prompt.trim());
}

export function switchRouterDraftMode(draft: RouterDraft, mode: RouterRoutingMode): RouterDraft {
  if (mode === draft.mode) return draft;
  if (mode === 'llm') {
    return { ...draft, mode: 'llm', rules: [], classifiers: [] };
  }
  return {
    ...draft,
    mode: 'rules',
    llmRouter: { model: '', prompt: '' },
    classifiers: [],
    rules: [createRouterRule(0, draft.defaultModel)],
  };
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

export function classifierLabels(classifier: RouterClassifier | undefined): string[] {
  if (!classifier) return [];
  return classifier.type === 'semantic_similarity'
    ? Object.keys(classifier.referencePhrases).filter(Boolean)
    : classifier.labels.filter(Boolean);
}

export function renameClassifierReference(node: RouterNode, previousId: string, nextId: string): RouterNode {
  if (node.kind === 'leaf') {
    return node.type === 'classifier' && node.classifierId === previousId
      ? { ...node, classifierId: nextId }
      : node;
  }
  return { ...node, children: node.children.map(child => renameClassifierReference(child, previousId, nextId)) };
}

export function renameClassifierLabelReference(
  node: RouterNode,
  classifierId: string,
  previousLabel: string,
  nextLabel: string,
): RouterNode {
  if (node.kind === 'leaf') {
    return node.type === 'classifier' && node.classifierId === classifierId && node.label === previousLabel
      ? { ...node, label: nextLabel }
      : node;
  }
  return {
    ...node,
    children: node.children.map(child => renameClassifierLabelReference(child, classifierId, previousLabel, nextLabel)),
  };
}

export function routerNodeReferencesClassifier(node: RouterNode, classifierId: string): boolean {
  if (node.kind === 'leaf') return node.type === 'classifier' && node.classifierId === classifierId;
  return node.children.some(child => routerNodeReferencesClassifier(child, classifierId));
}

export function normalizeRouterNode(node: RouterNode): RouterNode {
  if (node.kind === 'leaf') return node;
  const children = node.children.map(normalizeRouterNode).filter(Boolean);
  if (node.operator === 'not') {
    return { ...node, children: children.slice(0, 1).length ? children.slice(0, 1) : [createRouterLeaf()] };
  }
  if (children.length === 0) return createRouterLeaf();
  if (children.length === 1) return children[0];
  return { ...node, children };
}

function nodeToMatchExpression(node: RouterNode): Record<string, unknown> {
  if (node.kind === 'group') {
    const normalized = normalizeRouterNode(node);
    if (normalized.kind === 'leaf') return nodeToMatchExpression(normalized);
    if (normalized.operator === 'not') {
      return { not: nodeToMatchExpression(normalized.children[0]) };
    }
    return { [normalized.operator]: normalized.children.map(nodeToMatchExpression) };
  }

  switch (node.type) {
    case 'keywords_any': return { keywords_any: splitList(node.textValue) };
    case 'keywords_all': return { keywords_all: splitList(node.textValue) };
    case 'regex': return { regex: String(node.textValue || '').trim() };
    case 'min_chars': return { min_chars: Number(node.numberValue) };
    case 'max_chars': return { max_chars: Number(node.numberValue) };
    case 'has_tools': return { has_tools: node.booleanValue !== false };
    case 'has_images': return { has_images: node.booleanValue !== false };
    case 'classifier': {
      const result: Record<string, unknown> = { classifier: String(node.classifierId || '').trim() };
      if (node.label) result.label = node.label;
      if (node.minScore !== undefined) result.min_score = node.minScore;
      if (node.maxScore !== undefined) result.max_score = node.maxScore;
      return result;
    }
    case 'metadata': {
      const comparator = node.metadataComparator || 'equals';
      const metadata: Record<string, unknown> = { key: String(node.metadataKey || '').trim() };
      if (comparator === 'exists') metadata.exists = node.booleanValue !== false;
      else if (comparator === 'any') metadata.any = splitList(node.metadataValues);
      else metadata.equals = String(node.metadataValues ?? '');
      return { metadata };
    }
  }
}

function nestedUnboundedQuantifier(pattern: string): boolean {
  return /\((?:[^()\\]|\\.)*[+*](?:[^()\\]|\\.)*\)[+*]/.test(pattern);
}

function validateNode(
  node: RouterNode,
  classifiers: RouterClassifier[],
  path: string,
  depth: number,
  errors: string[],
): void {
  if (depth > MAX_ROUTER_TREE_DEPTH) {
    errors.push(`${path}: nesting exceeds ${MAX_ROUTER_TREE_DEPTH} levels.`);
    return;
  }
  if (node.kind === 'group') {
    if (node.operator === 'not' && node.children.length !== 1) errors.push(`${path}: NOT requires exactly one condition.`);
    if ((node.operator === 'all' || node.operator === 'any') && node.children.length === 0) errors.push(`${path}: ${node.operator.toUpperCase()} requires at least one condition.`);
    node.children.forEach((child, index) => validateNode(child, classifiers, `${path}.${node.operator}[${index}]`, depth + 1, errors));
    return;
  }

  const text = String(node.textValue ?? '').trim();
  if ((node.type === 'keywords_any' || node.type === 'keywords_all') && splitList(node.textValue).length === 0) {
    errors.push(`${path}: add at least one keyword.`);
  }
  if (node.type === 'regex') {
    if (!text) errors.push(`${path}: regex cannot be empty.`);
    else {
      try { new RegExp(text); } catch { errors.push(`${path}: regex is invalid.`); }
      if (nestedUnboundedQuantifier(text)) errors.push(`${path}: regex contains a nested unbounded quantifier rejected by the server.`);
    }
  }
  if (node.type === 'min_chars' || node.type === 'max_chars') {
    const value = Number(node.numberValue);
    if (!Number.isInteger(value) || value < 0) errors.push(`${path}: character bound must be a non-negative integer.`);
  }
  if (node.type === 'classifier') {
    const classifier = classifiers.find(item => item.id === node.classifierId);
    if (!classifier) {
      errors.push(`${path}: select a declared classifier.`);
      return;
    }
    const labels = classifierLabels(classifier);
    if (node.label && labels.length > 0 && !labels.includes(node.label)) {
      errors.push(`${path}: label "${node.label}" is not declared by classifier "${classifier.id}".`);
    }
    if (!node.label && labels.length > 0 && !classifier.defaultLabel) {
      errors.push(`${path}: select a label or configure a default label on classifier "${classifier.id}".`);
    }
    if (node.minScore !== undefined && (!Number.isFinite(node.minScore) || node.minScore < 0 || node.minScore > 1)) {
      errors.push(`${path}: min score must be in [0, 1].`);
    }
    if (node.maxScore !== undefined && (!Number.isFinite(node.maxScore) || node.maxScore < 0 || node.maxScore > 1)) {
      errors.push(`${path}: max score must be in [0, 1].`);
    }
    if (node.minScore !== undefined && node.maxScore !== undefined && node.minScore > node.maxScore) {
      errors.push(`${path}: min score cannot exceed max score.`);
    }
  }
  if (node.type === 'metadata') {
    if (!String(node.metadataKey || '').trim()) errors.push(`${path}: metadata key is required.`);
    if ((node.metadataComparator || 'equals') === 'any' && splitList(node.metadataValues).length === 0) {
      errors.push(`${path}: metadata "any" requires at least one value.`);
    }
  }
}

export function validateRouterDraft(draft: RouterDraft): string[] {
  const errors: string[] = [];
  const mode: RouterRoutingMode = draft.mode === 'llm' ? 'llm' : 'rules';
  if (!draft.name.trim() && !draft.modelName?.trim()) errors.push('Router name is required.');
  if (draft.candidates.length === 0) errors.push('Select at least one candidate model.');
  const candidateSet = new Set<string>();
  draft.candidates.forEach((candidate, index) => {
    const value = candidate.trim();
    if (!value) errors.push(`Candidate ${index + 1} is empty.`);
    if (candidateSet.has(value)) errors.push(`Candidate "${value}" is duplicated.`);
    candidateSet.add(value);
  });
  if (!draft.defaultModel || !candidateSet.has(draft.defaultModel)) errors.push('Default model must be one of the selected candidates.');

  if (mode === 'llm') {
    if (!String(draft.llmRouter?.model || '').trim()) errors.push('Natural-language router: model is required.');
    if (!String(draft.llmRouter?.prompt || '').trim()) errors.push('Natural-language router: routing instruction is required.');
    return errors;
  }

  const classifierIds = new Set<string>();
  draft.classifiers.forEach((classifier, index) => {
    const prefix = `Classifier ${index + 1}`;
    if (!classifier.id.trim()) errors.push(`${prefix}: ID is required.`);
    if (!SAFE_ROUTER_ID.test(classifier.id)) errors.push(`${prefix}: ID may contain only letters, numbers, dot, underscore, and hyphen.`);
    if (classifierIds.has(classifier.id)) errors.push(`${prefix}: duplicate ID "${classifier.id}".`);
    classifierIds.add(classifier.id);
    if (!classifier.model.trim()) errors.push(`${prefix}: model is required.`);
    if (classifier.type === 'classifier' || classifier.type === 'llm') {
      const labels = classifier.labels.map(label => label.trim()).filter(Boolean);
      if (classifier.type === 'llm' && labels.length === 0) errors.push(`${prefix}: add at least one output label.`);
      if (new Set(labels).size !== labels.length) errors.push(`${prefix}: labels must be unique.`);
      if (classifier.defaultLabel && !labels.includes(classifier.defaultLabel)) errors.push(`${prefix}: default label must be declared in labels.`);
      if (classifier.type === 'llm' && !classifier.prompt.trim()) errors.push(`${prefix}: prompt is required for an LLM classifier.`);
    } else {
      const concepts = Object.entries(classifier.referencePhrases);
      if (concepts.length === 0) errors.push(`${prefix}: add at least one semantic concept.`);
      for (const [concept, phrases] of concepts) {
        if (!concept.trim()) errors.push(`${prefix}: concept names cannot be empty.`);
        if (!phrases.map(item => item.trim()).filter(Boolean).length) errors.push(`${prefix}: concept "${concept}" needs at least one phrase.`);
      }
      const conceptLabels = concepts.map(([concept]) => concept);
      if (classifier.defaultLabel && !conceptLabels.includes(classifier.defaultLabel)) errors.push(`${prefix}: default label must be one of the concept names.`);
    }
  });

  if (draft.rules.length === 0) errors.push('Add at least one routing rule.');
  const ruleIds = new Set<string>();
  draft.rules.forEach((rule, index) => {
    const prefix = `Rule ${index + 1}`;
    if (!rule.id.trim()) errors.push(`${prefix}: ID is required.`);
    if (!SAFE_ROUTER_ID.test(rule.id)) errors.push(`${prefix}: ID may contain only letters, numbers, dot, underscore, and hyphen.`);
    if (ruleIds.has(rule.id)) errors.push(`${prefix}: duplicate ID "${rule.id}".`);
    ruleIds.add(rule.id);
    if (!candidateSet.has(rule.routeTo)) errors.push(`${prefix}: route target must be a selected candidate.`);
    if (rule.outputsText?.trim()) {
      try {
        const parsed = JSON.parse(rule.outputsText);
        if (!isRecord(parsed)) errors.push(`${prefix}: outputs JSON must be an object.`);
      } catch { errors.push(`${prefix}: outputs JSON is invalid.`); }
    }
    validateNode(rule.condition, draft.classifiers, prefix, 0, errors);
  });
  return errors;
}

export function buildRouterPullRequest(draft: RouterDraft): RouterPullRequest {
  const errors = validateRouterDraft(draft);
  if (errors.length) throw new Error(errors.slice(0, 6).join(' '));

  const components = new Set(draft.candidates);
  const mode: RouterRoutingMode = draft.mode === 'llm' ? 'llm' : 'rules';
  if (mode === 'llm') {
    const routerModel = draft.llmRouter.model.trim();
    components.add(routerModel);
    return {
      version: ROUTER_SCHEMA_VERSION,
      model_name: draft.modelName?.trim() || normalizeRouterModelName(draft.name),
      recipe: ROUTER_RECIPE,
      components: [...components].filter(Boolean),
      routing: {
        candidates: [...draft.candidates],
        default_model: draft.defaultModel,
        router: {
          type: 'llm',
          model: routerModel,
          prompt: draft.llmRouter.prompt.trim(),
        },
      },
    };
  }

  draft.classifiers.forEach(classifier => components.add(classifier.model));
  const classifiers = draft.classifiers.map(classifier => {
    const result: Record<string, unknown> = {
      id: classifier.id,
      type: classifier.type,
      model: classifier.model,
      on_error: classifier.onError,
    };
    if (classifier.type === 'classifier' || classifier.type === 'llm') {
      const labels = [...new Set(classifier.labels.map(label => label.trim()).filter(Boolean))];
      if (labels.length) result.labels = labels;
      if (classifier.type === 'llm') result.prompt = classifier.prompt.trim();
    } else {
      result.reference_phrases = Object.fromEntries(
        Object.entries(classifier.referencePhrases)
          .map(([concept, phrases]) => [concept.trim(), [...new Set(phrases.map(phrase => phrase.trim()).filter(Boolean))]])
          .filter(([concept, phrases]) => Boolean(concept) && (phrases as string[]).length > 0),
      );
    }
    if (classifier.defaultLabel) result.default_label = classifier.defaultLabel;
    return result;
  });
  const rules = draft.rules.map(rule => {
    const result: Record<string, unknown> = {
      id: rule.id,
      match: nodeToMatchExpression(normalizeRouterNode(rule.condition)),
      route_to: rule.routeTo,
    };
    if (rule.outputsText?.trim()) result.outputs = JSON.parse(rule.outputsText);
    return result;
  });
  const routing: RouterPullRequest['routing'] = {
    candidates: [...draft.candidates],
    default_model: draft.defaultModel,
    rules,
  };
  if (classifiers.length) routing.classifiers = classifiers;
  return {
    version: ROUTER_SCHEMA_VERSION,
    model_name: draft.modelName?.trim() || normalizeRouterModelName(draft.name),
    recipe: ROUTER_RECIPE,
    components: [...components].filter(Boolean),
    routing,
  };
}

function parseMatchExpression(expr: unknown): RouterNode {
  if (!isRecord(expr)) throw new Error('Rule match must be an object.');

  const logicalKeys = (['all', 'any', 'not'] as const).filter(key => key in expr);
  if (logicalKeys.length > 1) {
    throw new Error('Rule match must contain only one logical operator.');
  }
  if (logicalKeys.length === 1) {
    if (Object.keys(expr).length !== 1) {
      throw new Error('Rule match cannot mix logical operators with leaf conditions.');
    }
    const logicalKey = logicalKeys[0];
    if (logicalKey === 'not') {
      if (!isRecord(expr.not)) throw new Error('Rule match "not" must be an object.');
      return { id: createRouterNodeId('group'), kind: 'group', operator: 'not', children: [parseMatchExpression(expr.not)] };
    }
    const rawChildren = expr[logicalKey];
    if (!Array.isArray(rawChildren) || rawChildren.length === 0) {
      throw new Error(`Rule match "${logicalKey}" must be a non-empty array.`);
    }
    return {
      id: createRouterNodeId('group'),
      kind: 'group',
      operator: logicalKey,
      children: rawChildren.map(parseMatchExpression),
    };
  }

  const children: RouterNode[] = [];
  const consumed = new Set<string>();

  const parseStringArray = (key: 'keywords_any' | 'keywords_all'): string[] => {
    const value = expr[key];
    if (!Array.isArray(value) || value.length === 0) {
      throw new Error(`Rule match "${key}" must be a non-empty array.`);
    }
    if (value.some(item => typeof item !== 'string' || item.length === 0)) {
      throw new Error(`Rule match "${key}" items must be non-empty strings.`);
    }
    return value as string[];
  };

  if ('keywords_any' in expr) {
    children.push({ ...createRouterLeaf('keywords_any'), textValue: parseStringArray('keywords_any').join(', ') });
    consumed.add('keywords_any');
  }
  if ('keywords_all' in expr) {
    children.push({ ...createRouterLeaf('keywords_all'), textValue: parseStringArray('keywords_all').join(', ') });
    consumed.add('keywords_all');
  }
  if ('regex' in expr) {
    if (typeof expr.regex !== 'string' || !expr.regex.length) throw new Error('Rule match "regex" must be a non-empty string.');
    children.push({ ...createRouterLeaf('regex'), textValue: expr.regex });
    consumed.add('regex');
  }
  if ('min_chars' in expr) {
    if (typeof expr.min_chars !== 'number' || !Number.isInteger(expr.min_chars) || expr.min_chars < 0) throw new Error('Rule match "min_chars" must be a non-negative integer.');
    children.push({ ...createRouterLeaf('min_chars'), numberValue: expr.min_chars });
    consumed.add('min_chars');
  }
  if ('max_chars' in expr) {
    if (typeof expr.max_chars !== 'number' || !Number.isInteger(expr.max_chars) || expr.max_chars < 0) throw new Error('Rule match "max_chars" must be a non-negative integer.');
    children.push({ ...createRouterLeaf('max_chars'), numberValue: expr.max_chars });
    consumed.add('max_chars');
  }
  if ('has_tools' in expr) {
    if (typeof expr.has_tools !== 'boolean') throw new Error('Rule match "has_tools" must be a boolean.');
    children.push({ ...createRouterLeaf('has_tools'), booleanValue: expr.has_tools });
    consumed.add('has_tools');
  }
  if ('has_images' in expr) {
    if (typeof expr.has_images !== 'boolean') throw new Error('Rule match "has_images" must be a boolean.');
    children.push({ ...createRouterLeaf('has_images'), booleanValue: expr.has_images });
    consumed.add('has_images');
  }
  if ('classifier' in expr) {
    if (typeof expr.classifier !== 'string' || !expr.classifier.length) throw new Error('Rule match "classifier" must be a non-empty string.');
    if ('label' in expr && (typeof expr.label !== 'string' || !expr.label.length)) throw new Error('Rule match "label" must be a non-empty string.');
    if ('min_score' in expr && typeof expr.min_score !== 'number') throw new Error('Rule match "min_score" must be a number.');
    if ('max_score' in expr && typeof expr.max_score !== 'number') throw new Error('Rule match "max_score" must be a number.');
    children.push({
      ...createRouterLeaf('classifier'),
      classifierId: expr.classifier,
      label: typeof expr.label === 'string' ? expr.label : undefined,
      minScore: typeof expr.min_score === 'number' ? expr.min_score : undefined,
      maxScore: typeof expr.max_score === 'number' ? expr.max_score : undefined,
    });
    consumed.add('classifier');
    if ('label' in expr) consumed.add('label');
    if ('min_score' in expr) consumed.add('min_score');
    if ('max_score' in expr) consumed.add('max_score');
  } else if ('label' in expr || 'min_score' in expr || 'max_score' in expr) {
    throw new Error('Rule match classifier label/score fields require a "classifier" condition.');
  }
  if ('metadata' in expr) {
    if (!isRecord(expr.metadata)) throw new Error('Rule match "metadata" must be an object.');
    const metadata = expr.metadata;
    const allowedMetadataKeys = new Set(['key', 'equals', 'any', 'exists']);
    const unsupportedMetadata = Object.keys(metadata).filter(key => !allowedMetadataKeys.has(key));
    if (unsupportedMetadata.length > 0) {
      throw new Error(`Unsupported metadata field${unsupportedMetadata.length > 1 ? 's' : ''}: ${unsupportedMetadata.join(', ')}.`);
    }
    if (typeof metadata.key !== 'string' || !metadata.key.length) {
      throw new Error('Rule match metadata requires a non-empty string "key".');
    }
    const comparators = (['equals', 'any', 'exists'] as const).filter(key => key in metadata);
    if (comparators.length !== 1) {
      throw new Error('Rule match metadata requires exactly one comparator: equals, any, or exists.');
    }
    const comparator = comparators[0];
    if (comparator === 'equals' && typeof metadata.equals !== 'string') {
      throw new Error('Rule match metadata "equals" must be a string.');
    }
    if (comparator === 'any') {
      if (!Array.isArray(metadata.any) || metadata.any.length === 0) {
        throw new Error('Rule match metadata "any" must be a non-empty array.');
      }
      if (metadata.any.some(item => typeof item !== 'string' || item.length === 0)) {
        throw new Error('Rule match metadata "any" items must be non-empty strings.');
      }
    }
    if (comparator === 'exists' && typeof metadata.exists !== 'boolean') {
      throw new Error('Rule match metadata "exists" must be a boolean.');
    }
    children.push({
      ...createRouterLeaf('metadata'),
      metadataKey: metadata.key,
      metadataComparator: comparator,
      metadataValues: comparator === 'any'
        ? (metadata.any as string[]).join(', ')
        : comparator === 'equals' ? metadata.equals as string : '',
      booleanValue: comparator === 'exists' ? metadata.exists as boolean : undefined,
    });
    consumed.add('metadata');
  }

  const unsupported = Object.keys(expr).filter(key => !consumed.has(key));
  if (unsupported.length > 0) {
    throw new Error(`Unsupported rule condition field${unsupported.length > 1 ? 's' : ''}: ${unsupported.join(', ')}.`);
  }
  if (children.length === 0) throw new Error('Unsupported or empty rule condition.');
  if (children.length === 1) return children[0];
  return { id: createRouterNodeId('group'), kind: 'group', operator: 'all', children };
}

function parseClassifier(value: unknown, index: number): RouterClassifier {
  if (!isRecord(value)) throw new Error(`Classifier ${index + 1} must be an object.`);
  const type = String(value.type || '');
  if (type !== 'classifier' && type !== 'semantic_similarity' && type !== 'llm') {
    throw new Error(`Unsupported classifier type "${type}".`);
  }
  const referencePhrases: Record<string, string[]> = {};
  if (isRecord(value.reference_phrases)) {
    for (const [label, phrases] of Object.entries(value.reference_phrases)) referencePhrases[label] = splitList(phrases);
  }
  return {
    id: String(value.id || `classifier-${index + 1}`),
    type,
    model: String(value.model || ''),
    prompt: typeof value.prompt === 'string' ? value.prompt : '',
    labels: splitList(value.labels),
    defaultLabel: typeof value.default_label === 'string' ? value.default_label : undefined,
    referencePhrases,
    onError: value.on_error === 'match_true' ? 'match_true' : 'match_false',
  };
}

export function parseRouterPayload(payload: unknown): RouterDraft {
  const root = isRecord(payload) ? payload : null;
  if (!root) throw new Error('Router JSON must be an object.');
  if (root.recipe !== ROUTER_RECIPE) throw new Error(`Expected recipe "${ROUTER_RECIPE}".`);
  if (String(root.version || '') !== ROUTER_SCHEMA_VERSION) throw new Error(`Only router schema version ${ROUTER_SCHEMA_VERSION} is supported.`);
  const routing = isRecord(root.routing) ? root.routing : null;
  if (!routing) throw new Error('Router JSON is missing routing.');
  const candidates = splitList(routing.candidates);
  if ('router' in routing && !isRecord(routing.router)) {
    throw new Error('Natural-language router must be an object.');
  }
  const routerSpec = isRecord(routing.router) ? routing.router : null;
  if (routerSpec && ('rules' in routing || 'classifiers' in routing)) {
    throw new Error('Natural-language routing cannot be combined with rules or classifiers.');
  }
  if (routerSpec && routerSpec.type !== 'llm') throw new Error('Natural-language router type must be "llm".');

  const rulesRaw = routerSpec ? [] : Array.isArray(routing.rules) ? routing.rules : [];
  const classifiers = routerSpec
    ? []
    : (Array.isArray(routing.classifiers) ? routing.classifiers : []).map(parseClassifier);
  const rules = rulesRaw.map((item, index): RouterRule => {
    if (!isRecord(item)) throw new Error(`Rule ${index + 1} must be an object.`);
    return {
      id: String(item.id || `rule-${index + 1}`),
      routeTo: String(item.route_to || ''),
      condition: parseMatchExpression(item.match),
      outputsText: isRecord(item.outputs) ? JSON.stringify(item.outputs, null, 2) : '',
    };
  });
  const modelName = typeof root.model_name === 'string' ? root.model_name : undefined;
  const draft: RouterDraft = {
    modelName,
    name: routerDisplayName(modelName || 'router'),
    candidates,
    defaultModel: typeof routing.default_model === 'string' ? routing.default_model : '',
    mode: routerSpec ? 'llm' : 'rules',
    llmRouter: {
      model: routerSpec && typeof routerSpec.model === 'string' ? routerSpec.model : '',
      prompt: routerSpec && typeof routerSpec.prompt === 'string' ? routerSpec.prompt : '',
    },
    classifiers,
    rules: routerSpec ? [createRouterRule(0, typeof routing.default_model === 'string' ? routing.default_model : '')] : rules,
  };
  const errors = validateRouterDraft(draft);
  if (errors.length) throw new Error(errors.slice(0, 6).join(' '));
  return draft;
}

export function routerDraftFromModelInfo(model: ModelInfo): RouterDraft {
  return parseRouterPayload({
    version: String((model as any).version || ROUTER_SCHEMA_VERSION),
    model_name: String((model as any).model_name || model.name || model.id || ''),
    recipe: String((model as any).recipe || ''),
    components: Array.isArray((model as any).components) ? (model as any).components : [],
    routing: (model as any).routing,
  });
}
