export type RuleOperator = 'AND' | 'OR' | 'NOT';

// A leaf node references one condition signal.
export interface RuleLeaf {
  signalType: ConditionSignalType;
  // For keyword/regex/char signals: the value. For boolean signals: undefined.
  signalValue?: string | number;
  not?: boolean;
  // Classifier-specific extras
  classifierId?: string;
  label?: string;
  minScore?: number;
  maxScore?: number;
}

export type ConditionSignalType =
  | 'keywords_any'
  | 'keywords_all'
  | 'regex'
  | 'min_chars'
  | 'max_chars'
  | 'has_tools'
  | 'has_images'
  | 'classifier';

export interface RuleOperatorNode {
  operator: RuleOperator;
  conditions: RuleNode[];
}

export type RuleNode = RuleLeaf | RuleOperatorNode;

export type NodePath = number[];

export const DRAG_MIME = 'application/x-rule-tree';


export function isLeaf(n: RuleNode): n is RuleLeaf {
  return 'signalType' in n;
}

export function isOperatorNode(n: RuleNode): n is RuleOperatorNode {
  return 'operator' in n;
}


export type DragData =
  | { kind: 'operator'; operator: RuleOperator }
  | { kind: 'leaf'; leaf: RuleLeaf }
  | { kind: 'tree-node'; path: NodePath };

export function encodeDrag(data: DragData): string {
  return JSON.stringify(data);
}

export function decodeDrag(raw: string): DragData | null {
  try { return JSON.parse(raw) as DragData; } catch { return null; }
}


export function getNodeAtPath(root: RuleNode, path: NodePath): RuleNode | null {
  let cur: RuleNode = root;
  for (const idx of path) {
    if (!isOperatorNode(cur) || idx < 0 || idx >= cur.conditions.length) return null;
    cur = cur.conditions[idx];
  }
  return cur;
}

export function replaceAtPath(root: RuleNode, path: NodePath, replacement: RuleNode): RuleNode {
  if (path.length === 0) return replacement;
  if (!isOperatorNode(root)) return root;
  const [idx, ...rest] = path;
  const updated = replaceAtPath(root.conditions[idx], rest, replacement);
  return { ...root, conditions: root.conditions.map((c, i) => (i === idx ? updated : c)) };
}

function collapseIfSingle(node: RuleOperatorNode, next: RuleNode[]): RuleNode | null {
  if (next.length === 0) return null;
  return { ...node, conditions: next };
}

export function removeAtPath(root: RuleNode, path: NodePath): RuleNode | null {
  if (path.length === 0) return null;
  if (!isOperatorNode(root)) return root;
  const [idx, ...rest] = path;
  if (rest.length === 0) {
    return collapseIfSingle(root, root.conditions.filter((_, i) => i !== idx));
  }
  const updated = removeAtPath(root.conditions[idx], rest);
  if (!updated) {
    return collapseIfSingle(root, root.conditions.filter((_, i) => i !== idx));
  }
  return { ...root, conditions: root.conditions.map((c, i) => (i === idx ? updated : c)) };
}

export function addChildAtPath(root: RuleNode, path: NodePath, child: RuleNode): RuleNode {
  const target = getNodeAtPath(root, path);
  if (!target) return root;
  if (isOperatorNode(target)) {
    return replaceAtPath(root, path, { ...target, conditions: [...target.conditions, child] });
  }
  return replaceAtPath(root, path, { operator: 'AND', conditions: [target, child] });
}

export function insertAtPath(root: RuleNode, path: NodePath, insertIdx: number, node: RuleNode): RuleNode {
  if (path.length === 0) {
    if (isOperatorNode(root)) {
      const conditions = [...root.conditions];
      conditions.splice(insertIdx, 0, node);
      return { ...root, conditions };
    }
    return { operator: 'AND', conditions: [root, node] };
  }
  if (!isOperatorNode(root)) return root;
  const [idx, ...rest] = path;
  const updated = insertAtPath(root.conditions[idx], rest, insertIdx, node);
  return { ...root, conditions: root.conditions.map((c, i) => (i === idx ? updated : c)) };
}

export function pathEq(a: NodePath, b: NodePath): boolean {
  return a.length === b.length && a.every((v, i) => v === b[i]);
}

export function pathStartsWith(path: NodePath, prefix: NodePath): boolean {
  return prefix.length <= path.length && prefix.every((v, i) => v === path[i]);
}


function maybeNot(leaf: Record<string, unknown>, not: boolean | undefined): Record<string, unknown> {
  return not ? { not: leaf } : leaf;
}

export function ruleNodeToMatchExpr(node: RuleNode): Record<string, unknown> | null {
  if (isLeaf(node)) {
    const { signalType: t, signalValue: v, not, classifierId, label, minScore, maxScore } = node;
    if (t === 'keywords_any') {
      if (!v) return null;
      const kws = String(v).split(',').map(s => s.trim()).filter(Boolean);
      if (!kws.length) return null;
      return maybeNot({ keywords_any: kws }, not);
    }
    if (t === 'keywords_all') {
      if (!v) return null;
      const kws = String(v).split(',').map(s => s.trim()).filter(Boolean);
      if (!kws.length) return null;
      return maybeNot({ keywords_all: kws }, not);
    }
    if (t === 'regex') {
      if (!v || !String(v).trim()) return null;
      return maybeNot({ regex: String(v).trim() }, not);
    }
    if (t === 'min_chars') {
      if (v === undefined || v === '') return null;
      return maybeNot({ min_chars: Number(v) }, not);
    }
    if (t === 'max_chars') {
      if (v === undefined || v === '') return null;
      return maybeNot({ max_chars: Number(v) }, not);
    }
    if (t === 'has_tools') return maybeNot({ has_tools: true }, not);
    if (t === 'has_images') return maybeNot({ has_images: true }, not);
    if (t === 'classifier') {
      if (!classifierId) return null;
      const leaf: Record<string, unknown> = { classifier: classifierId };
      if (label) leaf.label = label;
      if (minScore !== undefined) leaf.min_score = minScore;
      if (maxScore !== undefined) leaf.max_score = maxScore;
      return maybeNot(leaf, not);
    }
    return null;
  }

  // Operator node
  if (node.operator === 'NOT') {
    const child = node.conditions[0];
    if (!child) return null;
    const childExpr = ruleNodeToMatchExpr(child);
    if (!childExpr) return null;
    return { not: childExpr };
  }

  const children = node.conditions
    .map(ruleNodeToMatchExpr)
    .filter((e): e is Record<string, unknown> => e !== null);

  if (children.length === 0) return null;
  if (children.length === 1) return children[0];

  const op = node.operator === 'AND' ? 'all' : 'any';
  return { [op]: children };
}


function leafFromExpr(expr: Record<string, unknown>, not?: boolean): RuleLeaf | null {
  if (Array.isArray(expr.keywords_any)) return { signalType: 'keywords_any', signalValue: (expr.keywords_any as string[]).join(', '), not };
  if (Array.isArray(expr.keywords_all)) return { signalType: 'keywords_all', signalValue: (expr.keywords_all as string[]).join(', '), not };
  if (typeof expr.regex === 'string') return { signalType: 'regex', signalValue: expr.regex, not };
  if (typeof expr.min_chars === 'number') return { signalType: 'min_chars', signalValue: expr.min_chars, not };
  if (typeof expr.max_chars === 'number') return { signalType: 'max_chars', signalValue: expr.max_chars, not };
  if ('has_tools' in expr) return expr.has_tools === false
    ? { signalType: 'has_tools', not: !not }
    : { signalType: 'has_tools', not };
  if ('has_images' in expr) return expr.has_images === false
    ? { signalType: 'has_images', not: !not }
    : { signalType: 'has_images', not };
  if (typeof expr.classifier === 'string') return {
    signalType: 'classifier',
    classifierId: expr.classifier,
    label: typeof expr.label === 'string' ? expr.label : undefined,
    minScore: typeof expr.min_score === 'number' ? expr.min_score : undefined,
    maxScore: typeof expr.max_score === 'number' ? expr.max_score : undefined,
    not,
  };
  return null;
}

export function matchExprToRuleNode(expr: Record<string, unknown>): RuleNode | null {
  if (!expr || typeof expr !== 'object' || Array.isArray(expr)) return null;

  // NOT wrapper
  if (expr.not && typeof expr.not === 'object' && !Array.isArray(expr.not)) {
    const inner = expr.not as Record<string, unknown>;
    // NOT around a leaf: toggle leaf.not, so not:{has_tools:false} means has_tools=true
    const leaf = leafFromExpr(inner);
    if (leaf) return { ...leaf, not: !leaf.not };
    // NOT around a subtree
    const child = matchExprToRuleNode(inner);
    if (child) return { operator: 'NOT', conditions: [child] };
    return null;
  }

  // AND / OR array
  for (const [jsonOp, ruleOp] of [['all', 'AND'], ['any', 'OR']] as const) {
    if (Array.isArray(expr[jsonOp])) {
      const children = (expr[jsonOp] as Record<string, unknown>[])
        .map(matchExprToRuleNode)
        .filter((n): n is RuleNode => n !== null);
      if (children.length === 0) return null;
      if (children.length === 1) return children[0];
      return { operator: ruleOp, conditions: children };
    }
  }

  // Flat leaf
  return leafFromExpr(expr);
}


const isPlainObject = (value: unknown): value is Record<string, unknown> => {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
};

// JSON serialization with sorted object keys, for order-insensitive comparison
// (nlohmann::json emits keys alphabetically; the UI emits insertion order).
export function stableStringify(value: unknown): string {
  if (Array.isArray(value)) {
    return '[' + value.map(stableStringify).join(',') + ']';
  }
  if (isPlainObject(value)) {
    return '{' + Object.keys(value).sort()
      .map(key => JSON.stringify(key) + ':' + stableStringify(value[key]))
      .join(',') + '}';
  }
  // JSON.stringify returns undefined for undefined/functions/symbols; map those
  // to 'null' so the function is total (values here come from parsed JSON, so
  // this branch is only a type-level concern).
  return JSON.stringify(value) ?? 'null';
}

// Trim and drop empty keywords - the whitespace normalization the editor's
// comma-separated input applies. Deliberately does NOT split on commas: a
// keyword containing a comma cannot survive the editor's round-trip, and that
// loss must stay detectable.
const normalizeKeywordArray = (items: unknown[]): string[] => {
  return items.map(String).map(s => s.trim()).filter(Boolean);
};

// Rewrite a match expression into the canonical form ruleNodeToMatchExpr emits,
// WITHOUT dropping anything the editor cannot represent. Only semantics-preserving
// rewrites happen here: {has_tools:false} ⇔ {not:{has_tools:true}}, double-negation
// collapse, single-child all/any unwrap, and keyword/regex whitespace normalization.
// Anything else (metadata leaves, compound leaves, unknown keys) passes through
// unchanged so a lossy round-trip still compares unequal.
export function canonicalizeMatchExpr(expr: unknown): unknown {
  if (!isPlainObject(expr)) return expr;
  const keys = Object.keys(expr);
  if (keys.length === 1) {
    const key = keys[0];
    if (key === 'not' && isPlainObject(expr.not)) {
      const inner = canonicalizeMatchExpr(expr.not);
      if (isPlainObject(inner) && Object.keys(inner).length === 1 && isPlainObject(inner.not)) {
        return inner.not;
      }
      return { not: inner };
    }
    if ((key === 'all' || key === 'any') && Array.isArray(expr[key])) {
      const children = (expr[key] as unknown[]).map(canonicalizeMatchExpr);
      if (children.length === 1) return children[0];
      return { [key]: children };
    }
    if ((key === 'has_tools' || key === 'has_images') && expr[key] === false) {
      return { not: { [key]: true } };
    }
  }
  const out: Record<string, unknown> = { ...expr };
  for (const kw of ['keywords_any', 'keywords_all'] as const) {
    if (Array.isArray(out[kw])) out[kw] = normalizeKeywordArray(out[kw] as unknown[]);
  }
  if (typeof out.regex === 'string') out.regex = (out.regex as string).trim();
  return out;
}

// True when re-serializing `tree` loses information from the original server-side
// match expression. Compares canonical semantics, not raw JSON text, so equivalent
// spellings ({has_tools:false} vs not-wrappers, key order, single-child gates) are
// not flagged.
export function matchExprRoundTripIsLossy(rawMatch: Record<string, unknown>, tree: RuleNode | null): boolean {
  const reserialized = tree ? ruleNodeToMatchExpr(tree) : null;
  return stableStringify(canonicalizeMatchExpr(reserialized ?? {})) !==
         stableStringify(canonicalizeMatchExpr(rawMatch));
}

export interface ValidatorClassifier {
  id: string;
  labels?: string[];
  defaultLabel?: string;
  referencePhrases?: Record<string, string[]>;
  type?: string;
}

const MAX_MATCH_EXPR_DEPTH = 64;

export function validateRuleNode(
  node: RuleNode | null,
  classifierIds: Set<string>,
  classifiers: ValidatorClassifier[] = [],
  depth = 0,
): string[] {
  if (!node) return [];
  if (depth >= MAX_MATCH_EXPR_DEPTH) return [`Condition tree is too deeply nested (max depth is ${MAX_MATCH_EXPR_DEPTH})`];
  if (isLeaf(node)) {
    if (node.signalType === 'keywords_any' || node.signalType === 'keywords_all') {
      const kws = String(node.signalValue ?? '').split(',').map(s => s.trim()).filter(Boolean);
      if (!kws.length) return [`A ${node.signalType} condition has no keywords`];
    }
    if (node.signalType === 'regex') {
      const pat = String(node.signalValue ?? '').trim();
      if (!pat) return ['A regex condition is empty'];
      try { new RegExp(pat); } catch { return [`Invalid regex pattern: ${pat}`]; }
      return [];
    }
    if (node.signalType === 'min_chars') {
      const v = node.signalValue;
      if (v === undefined || v === '') return ['A min_chars condition has no value'];
      const n = Number(v);
      if (!Number.isInteger(n) || n < 0) return [`min_chars must be a non-negative integer (got ${v})`];
    }
    if (node.signalType === 'max_chars') {
      const v = node.signalValue;
      if (v === undefined || v === '') return ['A max_chars condition has no value'];
      const n = Number(v);
      if (!Number.isInteger(n) || n < 0) return [`max_chars must be a non-negative integer (got ${v})`];
    }
    if (node.signalType === 'classifier') {
      if (!node.classifierId) return ['A classifier condition has no classifier selected'];
      if (!classifierIds.has(node.classifierId)) return [`Classifier "${node.classifierId}" is not declared`];
      // Score range checks
      const mn = node.minScore, mx = node.maxScore;
      if (mn !== undefined && (mn < 0 || mn > 1)) return [`Min score must be between 0 and 1 (got ${mn})`];
      if (mx !== undefined && (mx < 0 || mx > 1)) return [`Max score must be between 0 and 1 (got ${mx})`];
      if (mn !== undefined && mx !== undefined && mn > mx) {
        return [`Classifier score range is invalid: min (${mn}) > max (${mx})`];
      }
      // Label validity and defaultLabel requirement
      const clf = classifiers.find(c => c.id === node.classifierId);
      if (clf) {
        const validLabels = clf.type === 'semantic_similarity'
          ? Object.keys(clf.referencePhrases ?? {})
          : (clf.labels ?? []);
        if (node.label) {
          // Always validate an explicit label regardless of whether validLabels is empty -
          // an empty validLabels means the classifier is misconfigured, not that any label is ok.
          if (validLabels.length === 0) {
            return [`Classifier "${node.classifierId}" has no labels defined - remove the label from this condition or add labels to the classifier`];
          }
          if (!validLabels.includes(node.label)) {
            return [`Classifier "${node.classifierId}" has no label "${node.label}" - it may have been renamed or removed`];
          }
        } else if (validLabels.length > 0) {
          // No explicit label - require a valid defaultLabel.
          // Exception: a single-concept semantic_similarity classifier implicitly uses its only concept.
          const effectiveDefault = clf.defaultLabel || (clf.type === 'semantic_similarity' && validLabels.length === 1 ? validLabels[0] : undefined);
          if (!effectiveDefault) {
            return [`Classifier "${node.classifierId}" has no default label - either select a label or set a default in the classifier config`];
          }
          if (!validLabels.includes(effectiveDefault)) {
            return [`Classifier "${node.classifierId}" default label "${effectiveDefault}" is no longer valid - update the classifier config`];
          }
        }
      }
    }
    return [];
  }
  if (node.operator === 'NOT' && node.conditions.length !== 1) return ['NOT must have exactly one child'];
  if ((node.operator === 'AND' || node.operator === 'OR') && node.conditions.length < 2) return [`${node.operator} needs at least 2 children`];
  return node.conditions.flatMap(c => validateRuleNode(c, classifierIds, classifiers, depth + 1));
}

// Signals that may appear at most once as a direct child of any single gate
export const SINGLETON_SIGNALS = new Set<ConditionSignalType>([
  'keywords_any', 'keywords_all', 'min_chars', 'max_chars', 'has_tools', 'has_images',
]);


export function collectUsedSignalTypes(node: RuleNode | null): Set<ConditionSignalType> {
  const used = new Set<ConditionSignalType>();
  const walk = (n: RuleNode) => {
    if (isLeaf(n)) { used.add(n.signalType); return; }
    n.conditions.forEach(walk);
  };
  if (node) walk(node);
  return used;
}


export const SIGNAL_LABELS: Record<ConditionSignalType, string> = {
  keywords_any: 'Keywords (any)',
  keywords_all: 'Keywords (all)',
  regex: 'Regex',
  min_chars: 'Min chars',
  max_chars: 'Max chars',
  has_tools: 'Has tools',
  has_images: 'Has images',
  classifier: 'Classifier',
};

export const SIGNAL_COLORS: Record<ConditionSignalType, string> = {
  keywords_any: '#4a9eff',
  keywords_all: '#38B2AC',
  regex: '#9F7AEA',
  min_chars: '#4CAF50',
  max_chars: '#4CAF50',
  has_tools: '#ED8936',
  has_images: '#D69E2E',
  classifier: '#F56565',
};

export const OPERATOR_COLORS: Record<RuleOperator, string> = {
  AND: '#38B2AC',
  OR: '#ED8936',
  NOT: '#F56565',
};

export function leafSummary(leaf: RuleLeaf): string {
  const t = leaf.signalType;
  if (t === 'keywords_any' || t === 'keywords_all') {
    const kws = String(leaf.signalValue ?? '').split(',').map(s => s.trim()).filter(Boolean);
    return kws.length ? kws.slice(0, 3).join(', ') + (kws.length > 3 ? '…' : '') : '(empty)';
  }
  if (t === 'regex') return String(leaf.signalValue || '(empty)');
  if (t === 'min_chars') return `≥ ${leaf.signalValue ?? '?'}`;
  if (t === 'max_chars') return `≤ ${leaf.signalValue ?? '?'}`;
  if (t === 'has_tools') return 'has tools';
  if (t === 'has_images') return 'has images';
  if (t === 'classifier') return `${leaf.classifierId ?? '?'} ≥ ${leaf.minScore ?? 0.5}`;
  return '';
}

export function makeDefaultLeaf(signalType: ConditionSignalType, classifierId?: string): RuleLeaf {
  const base: RuleLeaf = { signalType };
  if (signalType === 'keywords_any' || signalType === 'keywords_all') base.signalValue = '';
  if (signalType === 'regex') base.signalValue = '';
  if (signalType === 'min_chars') base.signalValue = 500;
  if (signalType === 'max_chars') base.signalValue = 2000;
  if (signalType === 'classifier') { base.classifierId = classifierId ?? ''; base.minScore = 0.5; }
  return base;
}
