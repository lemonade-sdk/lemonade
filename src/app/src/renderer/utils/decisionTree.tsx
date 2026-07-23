import React from 'react';

export interface PolicyRule {
  id: string;
  match?: unknown;
  route_to: string;
}

export interface RoutingPolicyDoc {
  routing?: {
    rules?: PolicyRule[];
    default_model?: string;
  };
}

export interface TraceEntry {
  condition: string;
  score?: number;
  result: boolean;
  label?: string;
  rationale?: string;
}

export interface DecisionResult {
  route_to: string;
  matched_rule: string;
  default_used: boolean;
  outputs?: Record<string, unknown>;
  trace: TraceEntry[];
}

const truncate = (text: string, maxLen: number): string =>
  text.length <= maxLen ? text : `${text.slice(0, maxLen - 1)}…`;

/**
 * Best-effort textual rendering of a routing rule's match expression, for
 * the decision-tree node label. Not a full pretty-printer — deeply nested
 * expressions are summarized, not exhaustively rendered; see the full
 * policy JSON for the exact expression.
 */
export const summarizeMatchExpr = (expr: unknown): string => {
  if (expr === null || expr === undefined) return '(none)';
  if (typeof expr !== 'object') return String(expr);
  const obj = expr as Record<string, unknown>;

  if (Array.isArray(obj.any)) {
    return `any(${obj.any.map(summarizeMatchExpr).join(', ')})`;
  }
  if (Array.isArray(obj.all)) {
    return `all(${obj.all.map(summarizeMatchExpr).join(', ')})`;
  }
  if ('not' in obj) {
    return `not(${summarizeMatchExpr(obj.not)})`;
  }

  // Classifier-band leaves (`{classifier, label?, min_score?, max_score?}`) carry
  // several fields that jointly determine the match — falling through to the
  // generic single-key rendering below would show only `classifier: <id>` and
  // silently drop the label/threshold that actually decide whether it matches.
  if (typeof obj.classifier === 'string') {
    const parts = [`classifier: ${obj.classifier}`];
    if (typeof obj.label === 'string') {
      parts.push(`label: ${obj.label}`);
    }
    const hasMin = typeof obj.min_score === 'number';
    const hasMax = typeof obj.max_score === 'number';
    if (hasMin && hasMax) {
      parts.push(`score: [${obj.min_score}, ${obj.max_score}]`);
    } else if (hasMin) {
      parts.push(`score ≥ ${obj.min_score}`);
    } else if (hasMax) {
      parts.push(`score ≤ ${obj.max_score}`);
    } else {
      // Frozen v1 default when neither bound is given — see routing_policy.cpp.
      parts.push('score ≥ 0.5 (default)');
    }
    return parts.join(', ');
  }

  // A leaf may carry more than one deterministic condition (e.g.
  // `{"min_chars": 4000, "has_tools": true}`), which the engine compiles as
  // an implicit `all` — every key must hold for the leaf to match. Render
  // every entry, not just the first, joined with an explicit AND so the tree/
  // tooltip/accessible label don't silently drop conditions that co-decide
  // the match.
  const entries = Object.entries(obj);
  if (entries.length === 0) return '(empty)';
  const formatEntry = ([key, value]: [string, unknown]): string => {
    if (value === undefined) return key;
    if (Array.isArray(value)) return `${key}: ${value.join(', ')}`;
    if (typeof value === 'object') return `${key}: ${JSON.stringify(value)}`;
    return `${key}: ${value}`;
  };
  return entries.map(formatEntry).join(' AND ');
};

/** The request inputs that determined a decision — carried alongside the
 * decision itself so the trace export and decision tree can reproduce
 * exactly what was sent, not just the prompt. */
export interface TraceRequestInputs {
  prompt: string;
  hasImages: boolean;
  hasTools: boolean;
  metadata: Record<string, string>;
  policyFilename: string | null;
}

/** `"key=value, key2=value2"`, or `"(none)"` for an empty map. Shared by the
 * text trace export and the decision-tree header so the two never drift. */
const formatMetadataSummary = (metadata: Record<string, string>): string => {
  const entries = Object.entries(metadata);
  return entries.length === 0 ? '(none)' : entries.map(([key, value]) => `${key}=${value}`).join(', ');
};

/** Formats one trace entry as a single line, shared by the visible trace log
 * and the downloaded trace file so neither can drift from the other. */
export const formatTraceEntry = (entry: TraceEntry, index: number): string => {
  const parts = [`${index + 1}. ${entry.condition}:`];
  if (entry.score !== undefined) parts.push(`score=${entry.score.toFixed(2)}`);
  if (entry.label) parts.push(`label=${entry.label}`);
  if (entry.rationale) parts.push(`rationale="${entry.rationale}"`);
  parts.push(`result=${entry.result}`);
  return parts.join(' ');
};

/** Plain-text trace export for the "Download trace" button. */
export const formatTraceAsText = (decision: DecisionResult, request: TraceRequestInputs): string => {
  const lines: string[] = [];
  lines.push('Lemonade Prompt Debugger — Trace Export');
  lines.push(`Policy: ${request.policyFilename ?? '(unsaved policy)'}`);
  lines.push(`Prompt: ${request.prompt.replace(/\r?\n/g, '\\n')}`);
  lines.push(`has_images: ${request.hasImages}`);
  lines.push(`has_tools: ${request.hasTools}`);
  lines.push(`metadata: ${formatMetadataSummary(request.metadata)}`);
  lines.push(`route_to: ${decision.route_to}`);
  lines.push(`matched_rule: ${decision.matched_rule || '(none)'}`);
  lines.push(`default_used: ${decision.default_used}`);
  lines.push('');
  lines.push('Trace:');
  decision.trace.forEach((entry, i) => {
    lines.push(formatTraceEntry(entry, i));
  });
  return lines.join('\n');
};

/** Trigger a browser download of the trace as a plain-text file. */
export const downloadTraceFile = (decision: DecisionResult, request: TraceRequestInputs): void => {
  const text = formatTraceAsText(decision, request);
  const blob = new Blob([text], { type: 'text/plain' });
  const url = window.URL.createObjectURL(blob);
  const safeRule = (decision.matched_rule || 'default').replace(/[^a-zA-Z0-9_-]+/g, '-');
  const link = document.createElement('a');
  link.href = url;
  link.download = `routing-trace-${safeRule}-${Date.now()}.txt`;
  document.body.appendChild(link);
  link.click();
  link.remove();
  window.URL.revokeObjectURL(url);
};

const RULE_BOX_WIDTH = 260;
const RULE_BOX_HEIGHT = 50;
const ROW_GAP = 40;
const CANDIDATE_GAP = 70;
const CANDIDATE_BOX_WIDTH = 150;
const CANDIDATE_BOX_HEIGHT = 30;
const MARGIN = 20;
const HEADER_LINE_HEIGHT = 16;
const HEADER_LINE_MAX_CHARS = 90;

const RED = 'var(--toast-error-text)';
const GRAY = 'var(--border-6)';

/**
 * Index of the matched rule within `policy.routing.rules` (or `rules.length`
 * for the trailing Default node). Returns -1 when `decision.matched_rule`
 * names a rule id that isn't present in `policy` — e.g. the policy was
 * swapped/edited client-side after the decision was returned.
 */
export const findMatchedRuleIndex = (policy: RoutingPolicyDoc, decision: DecisionResult): number => {
  const rules = policy.routing?.rules ?? [];
  return decision.default_used ? rules.length : rules.findIndex((r) => r.id === decision.matched_rule);
};

/**
 * Hand-rolled SVG decision-tree: one node per top-level rule (in policy
 * order) plus a trailing Default node. Since routing is first-match-wins,
 * knowing which rule matched (or that the default was used) is enough to
 * color the taken path red and dim every rule after it, with no need to
 * attribute individual trace entries back to specific rules.
 */
export const renderDecisionTree = (
  policy: RoutingPolicyDoc,
  decision: DecisionResult,
  request: TraceRequestInputs
): JSX.Element => {
  const rules = policy.routing?.rules ?? [];
  const defaultModel = policy.routing?.default_model ?? '(none)';
  const matchedIndex = findMatchedRuleIndex(policy, decision);

  // Header block: the request context that produced this tree, so the image
  // is self-contained (policy filename, prompt, and flags/metadata) without
  // needing the surrounding panel or a separately-downloaded trace file.
  const headerLines = [
    `Policy: ${truncate(request.policyFilename ?? '(unsaved policy)', HEADER_LINE_MAX_CHARS)}`,
    `Prompt: ${truncate(request.prompt.replace(/\r?\n/g, ' '), HEADER_LINE_MAX_CHARS)}`,
    `has_images: ${request.hasImages}    has_tools: ${request.hasTools}`,
    `metadata: ${truncate(formatMetadataSummary(request.metadata), HEADER_LINE_MAX_CHARS)}`,
  ];
  const headerTexts = headerLines.map((line, i) => (
    <text key={`header-${i}`} x={MARGIN} y={MARGIN + i * HEADER_LINE_HEIGHT + 10} fontSize={11} fill="var(--text-secondary)">
      {line}
    </text>
  ));
  // Rule rows start below the header block, offset by its height plus a gap;
  // every row/edge/default-node `y` and `svgHeight` below is measured from
  // this instead of the bare MARGIN they used before the header existed.
  const headerOffset = MARGIN + headerLines.length * HEADER_LINE_HEIGHT + MARGIN;

  const rowHeight = RULE_BOX_HEIGHT + ROW_GAP;
  const svgWidth = MARGIN + RULE_BOX_WIDTH + CANDIDATE_GAP + CANDIDATE_BOX_WIDTH + MARGIN;
  const svgHeight = headerOffset + (rules.length + 1) * rowHeight;

  // A transition out of rule i ("no match, fall through") only actually
  // happened if rule i was evaluated and failed, i.e. i < matchedIndex.
  // (When matchedIndex is -1 — the matched_rule id wasn't found among the
  // rules, an inconsistent-but-non-fatal edge case — nothing is highlighted.)
  const edgeOccurred = (i: number) => (matchedIndex === -1 ? true : i < matchedIndex);

  const nodes: JSX.Element[] = [];
  const edges: JSX.Element[] = [];

  rules.forEach((rule, i) => {
    const y = headerOffset + i * rowHeight;
    const isWinner = matchedIndex === i;
    const isUnreached = matchedIndex !== -1 && i > matchedIndex;
    const nodeOpacity = isUnreached ? 0.35 : 1;
    const nodeStroke = isWinner ? RED : GRAY;

    const fullMatchExpr = summarizeMatchExpr(rule.match);
    const ruleStatusLabel = isWinner
      ? 'matched'
      : isUnreached
      ? 'not evaluated — an earlier rule already matched'
      : 'evaluated — did not match';
    const ruleLabel = `Rule "${rule.id}": routes to "${rule.route_to}" when ${fullMatchExpr} (${ruleStatusLabel})`;

    nodes.push(
      <g key={`rule-${rule.id}-${i}`} opacity={nodeOpacity} aria-label={ruleLabel}>
        <title>{ruleLabel}</title>
        <rect
          x={MARGIN} y={y} width={RULE_BOX_WIDTH} height={RULE_BOX_HEIGHT} rx={6}
          fill="var(--bg-secondary)" stroke={nodeStroke} strokeWidth={isWinner ? 2 : 1.5}
        />
        <text x={MARGIN + 10} y={y + 20} fontSize={11} fontWeight={600} fill="var(--text-primary)">
          {truncate(rule.id, 34)}
        </text>
        <text x={MARGIN + 10} y={y + 37} fontSize={10} fill="var(--text-secondary)">
          {truncate(fullMatchExpr, 42)}
        </text>
      </g>
    );

    const candX = MARGIN + RULE_BOX_WIDTH + CANDIDATE_GAP;
    const candY = y + (RULE_BOX_HEIGHT - CANDIDATE_BOX_HEIGHT) / 2;
    const matchOccurred = matchedIndex === -1 ? false : isWinner;
    const matchColor = matchOccurred ? RED : GRAY;
    const matchOpacity = isUnreached ? 0.35 : 1;
    const candidateLabel = `Routes to "${rule.route_to}"`;

    edges.push(
      <g key={`match-edge-${rule.id}-${i}`} opacity={matchOpacity} aria-label={candidateLabel}>
        <title>{candidateLabel}</title>
        <line
          x1={MARGIN + RULE_BOX_WIDTH} y1={y + RULE_BOX_HEIGHT / 2}
          x2={candX} y2={candY + CANDIDATE_BOX_HEIGHT / 2}
          stroke={matchColor} strokeWidth={matchOccurred ? 2.5 : 1.5}
          markerEnd={matchOccurred ? 'url(#decision-tree-arrow-red)' : 'url(#decision-tree-arrow-gray)'}
        />
        <rect
          x={candX} y={candY} width={CANDIDATE_BOX_WIDTH} height={CANDIDATE_BOX_HEIGHT} rx={5}
          fill="none" stroke={matchColor} strokeWidth={matchOccurred ? 2 : 1}
        />
        <text x={candX + 8} y={candY + 19} fontSize={10} fill="var(--text-primary)">
          {truncate(rule.route_to, 20)}
        </text>
      </g>
    );

    const nextY = headerOffset + (i + 1) * rowHeight;
    const isFinalToDefault = i === rules.length - 1;
    const noMatchIsSelected = decision.default_used && isFinalToDefault;
    const noMatchColor = noMatchIsSelected ? RED : GRAY;
    edges.push(
      <g key={`no-match-${i}`} opacity={edgeOccurred(i) ? 1 : 0.35}>
        <line
          x1={MARGIN + RULE_BOX_WIDTH / 2} y1={y + RULE_BOX_HEIGHT}
          x2={MARGIN + RULE_BOX_WIDTH / 2} y2={nextY}
          stroke={noMatchColor} strokeWidth={noMatchIsSelected ? 2.5 : 1.5}
          markerEnd={noMatchIsSelected ? 'url(#decision-tree-arrow-red)' : 'url(#decision-tree-arrow-gray)'}
        />
        <text
          x={MARGIN + RULE_BOX_WIDTH / 2 + 6} y={y + RULE_BOX_HEIGHT + ROW_GAP / 2 + 3}
          fontSize={9} fill="var(--text-tertiary)"
        >
          no match
        </text>
      </g>
    );
  });

  const defaultY = headerOffset + rules.length * rowHeight;
  const defaultIsWinner = matchedIndex === rules.length;
  const defaultLabel = `Default fallback: routes to "${defaultModel}"${defaultIsWinner ? ' (used — no rule matched)' : ''}`;
  nodes.push(
    <g key="default-node" aria-label={defaultLabel}>
      <title>{defaultLabel}</title>
      <rect
        x={MARGIN} y={defaultY} width={RULE_BOX_WIDTH} height={RULE_BOX_HEIGHT} rx={6}
        fill="var(--bg-secondary)" stroke={defaultIsWinner ? RED : GRAY}
        strokeWidth={defaultIsWinner ? 2 : 1.5} strokeDasharray="4 3"
      />
      <text x={MARGIN + 10} y={defaultY + 20} fontSize={11} fontWeight={600} fill="var(--text-primary)">
        Default
      </text>
      <text x={MARGIN + 10} y={defaultY + 37} fontSize={10} fill="var(--text-secondary)">
        {`→ ${truncate(defaultModel, 34)}`}
      </text>
    </g>
  );

  const rulesCount = rules.length;
  const contextSummary =
    `Policy "${request.policyFilename ?? '(unsaved policy)'}", prompt "${truncate(request.prompt.replace(/\r?\n/g, ' '), HEADER_LINE_MAX_CHARS)}", ` +
    `has_images=${request.hasImages}, has_tools=${request.hasTools}, metadata: ${formatMetadataSummary(request.metadata)}. `;
  const decisionSummary =
    contextSummary +
    (decision.default_used
      ? `Routing decision tree: ${rulesCount} rule${rulesCount === 1 ? '' : 's'} evaluated, none matched; fell through to the default model "${defaultModel}".`
      : matchedIndex === -1
      ? `Routing decision tree: matched rule "${decision.matched_rule}" was not found among the ${rulesCount} rule${rulesCount === 1 ? '' : 's'} in this policy.`
      : `Routing decision tree: rule "${decision.matched_rule}" matched and routed to "${decision.route_to}".`);

  return (
    <svg
      className="decision-tree-svg"
      viewBox={`0 0 ${svgWidth} ${svgHeight}`}
      width={svgWidth}
      height={svgHeight}
      xmlns="http://www.w3.org/2000/svg"
      role="img"
      aria-label={decisionSummary}
    >
      <title>{decisionSummary}</title>
      <defs>
        <marker id="decision-tree-arrow-gray" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto-start-reverse">
          <path d="M0,0 L10,5 L0,10 z" fill="var(--border-6)" />
        </marker>
        <marker id="decision-tree-arrow-red" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto-start-reverse">
          <path d="M0,0 L10,5 L0,10 z" fill="var(--toast-error-text)" />
        </marker>
      </defs>
      {headerTexts}
      {edges}
      {nodes}
    </svg>
  );
};
