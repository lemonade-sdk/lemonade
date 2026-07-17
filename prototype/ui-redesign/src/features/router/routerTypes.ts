import type { ModelInfo } from '../../api';

export const ROUTER_RECIPE = 'collection.router' as const;
export const ROUTER_SCHEMA_VERSION = '1' as const;
export const SAFE_ROUTER_ID = /^[A-Za-z0-9._-]+$/;
export const MAX_ROUTER_TREE_DEPTH = 64;

export type RouterClassifierType = 'classifier' | 'semantic_similarity';
export type RouterOnError = 'match_true' | 'match_false';
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
  labels: string[];
  defaultLabel?: string;
  referencePhrases: Record<string, string[]>;
  onError: RouterOnError;
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
    classifiers?: Array<Record<string, unknown>>;
    rules: Array<Record<string, unknown>>;
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
    labels: type === 'classifier' ? ['match'] : [],
    defaultLabel: type === 'classifier' ? 'match' : undefined,
    referencePhrases: type === 'semantic_similarity' ? { concept: ['example phrase'] } : {},
    onError: 'match_false',
  };
}

export function createEmptyRouterDraft(): RouterDraft {
  return {
    name: '',
    candidates: [],
    defaultModel: '',
    classifiers: [],
    rules: [createRouterRule(0)],
  };
}

function splitList(value: unknown): string[] {
  if (Array.isArray(value)) return value.map(item => String(item).trim()).filter(Boolean);
  return String(value ?? '').split(',').map(item => item.trim()).filter(Boolean);
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

  const classifierIds = new Set<string>();
  draft.classifiers.forEach((classifier, index) => {
    const prefix = `Classifier ${index + 1}`;
    if (!classifier.id.trim()) errors.push(`${prefix}: ID is required.`);
    if (!SAFE_ROUTER_ID.test(classifier.id)) errors.push(`${prefix}: ID may contain only letters, numbers, dot, underscore, and hyphen.`);
    if (classifierIds.has(classifier.id)) errors.push(`${prefix}: duplicate ID "${classifier.id}".`);
    classifierIds.add(classifier.id);
    if (!classifier.model.trim()) errors.push(`${prefix}: model is required.`);
    if (classifier.type === 'classifier') {
      const labels = classifier.labels.map(label => label.trim()).filter(Boolean);
      if (new Set(labels).size !== labels.length) errors.push(`${prefix}: labels must be unique.`);
      if (classifier.defaultLabel && !labels.includes(classifier.defaultLabel)) errors.push(`${prefix}: default label must be declared in labels.`);
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
  draft.classifiers.forEach(classifier => components.add(classifier.model));
  const classifiers = draft.classifiers.map(classifier => {
    const result: Record<string, unknown> = {
      id: classifier.id,
      type: classifier.type,
      model: classifier.model,
      on_error: classifier.onError,
    };
    if (classifier.type === 'classifier') {
      const labels = [...new Set(classifier.labels.map(label => label.trim()).filter(Boolean))];
      if (labels.length) result.labels = labels;
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
  if (Array.isArray(expr.all)) {
    return { id: createRouterNodeId('group'), kind: 'group', operator: 'all', children: expr.all.map(parseMatchExpression) };
  }
  if (Array.isArray(expr.any)) {
    return { id: createRouterNodeId('group'), kind: 'group', operator: 'any', children: expr.any.map(parseMatchExpression) };
  }
  if (isRecord(expr.not)) {
    return { id: createRouterNodeId('group'), kind: 'group', operator: 'not', children: [parseMatchExpression(expr.not)] };
  }
  if (Array.isArray(expr.keywords_any)) return { ...createRouterLeaf('keywords_any'), textValue: expr.keywords_any.join(', ') };
  if (Array.isArray(expr.keywords_all)) return { ...createRouterLeaf('keywords_all'), textValue: expr.keywords_all.join(', ') };
  if (typeof expr.regex === 'string') return { ...createRouterLeaf('regex'), textValue: expr.regex };
  if (typeof expr.min_chars === 'number') return { ...createRouterLeaf('min_chars'), numberValue: expr.min_chars };
  if (typeof expr.max_chars === 'number') return { ...createRouterLeaf('max_chars'), numberValue: expr.max_chars };
  if (typeof expr.has_tools === 'boolean') return { ...createRouterLeaf('has_tools'), booleanValue: expr.has_tools };
  if (typeof expr.has_images === 'boolean') return { ...createRouterLeaf('has_images'), booleanValue: expr.has_images };
  if (typeof expr.classifier === 'string') {
    return {
      ...createRouterLeaf('classifier'),
      classifierId: expr.classifier,
      label: typeof expr.label === 'string' ? expr.label : undefined,
      minScore: typeof expr.min_score === 'number' ? expr.min_score : undefined,
      maxScore: typeof expr.max_score === 'number' ? expr.max_score : undefined,
    };
  }
  if (isRecord(expr.metadata)) {
    const metadata = expr.metadata;
    let comparator: RouterMetadataComparator = 'equals';
    if ('any' in metadata) comparator = 'any';
    if ('exists' in metadata) comparator = 'exists';
    return {
      ...createRouterLeaf('metadata'),
      metadataKey: typeof metadata.key === 'string' ? metadata.key : '',
      metadataComparator: comparator,
      metadataValues: comparator === 'any' && Array.isArray(metadata.any)
        ? metadata.any.join(', ')
        : comparator === 'equals' ? String(metadata.equals ?? '') : '',
      booleanValue: comparator === 'exists' ? metadata.exists !== false : undefined,
    };
  }
  throw new Error('Unsupported or empty rule condition.');
}

function parseClassifier(value: unknown, index: number): RouterClassifier {
  if (!isRecord(value)) throw new Error(`Classifier ${index + 1} must be an object.`);
  const type = String(value.type || '');
  if (type === 'llm') throw new Error('LLM classifiers are not supported by the current Lemonade server.');
  if (type !== 'classifier' && type !== 'semantic_similarity') throw new Error(`Unsupported classifier type "${type}".`);
  const referencePhrases: Record<string, string[]> = {};
  if (isRecord(value.reference_phrases)) {
    for (const [label, phrases] of Object.entries(value.reference_phrases)) referencePhrases[label] = splitList(phrases);
  }
  return {
    id: String(value.id || `classifier-${index + 1}`),
    type,
    model: String(value.model || ''),
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
  if (routing.router) throw new Error('NL/LLM router sugar is not supported by the current Lemonade server. Convert it to explicit rules first.');
  const candidates = splitList(routing.candidates);
  const rulesRaw = Array.isArray(routing.rules) ? routing.rules : [];
  const classifiers = (Array.isArray(routing.classifiers) ? routing.classifiers : []).map(parseClassifier);
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
    classifiers,
    rules,
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
