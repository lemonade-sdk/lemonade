import {
  createRouterLeaf,
  createRouterNodeId,
  normalizeRouterNode,
  routerConditionIdentity,
  type RouterGroupNode,
  type RouterGroupOperator,
  type RouterNode,
} from './routerTypes';

export type RouterNodePath = number[];

export function routerNodeAtPath(root: RouterNode, path: RouterNodePath): RouterNode | null {
  let current: RouterNode = root;
  for (const index of path) {
    if (current.kind !== 'group' || index < 0 || index >= current.children.length) return null;
    current = current.children[index];
  }
  return current;
}

function duplicateConditionMessage(node: RouterNode): string {
  if (node.kind !== 'leaf') return 'This condition is already present in this gate.';
  return `A ${node.type.replace(/_/g, ' ')} condition is already in this gate.`;
}

function duplicateConditionInChildren(children: RouterNode[], candidate: RouterNode, ignoreIndex = -1): string | null {
  const identity = routerConditionIdentity(candidate);
  if (!identity) return null;
  const duplicate = children.some((child, index) => index !== ignoreIndex && routerConditionIdentity(child) === identity);
  return duplicate ? duplicateConditionMessage(candidate) : null;
}

function duplicateConditionInSubtree(node: RouterNode): string | null {
  if (node.kind !== 'group') return null;
  for (let index = 0; index < node.children.length; index += 1) {
    const child = node.children[index];
    const conflict = duplicateConditionInChildren(node.children, child, index);
    if (conflict) return conflict;
    const nestedConflict = duplicateConditionInSubtree(child);
    if (nestedConflict) return nestedConflict;
  }
  return null;
}

export function routerDuplicateConditionConflict(node: RouterNode): string | null {
  return duplicateConditionInSubtree(node);
}

/**
 * Returns a user-facing reason when replacing a node would create two direct
 * singleton conditions inside one gate. Parameterized conditions such as regex,
 * classifier bands, and metadata predicates remain repeatable because they can
 * express different constraints and cannot be merged losslessly.
 */
export function routerReplacementConflictAtPath(
  root: RouterNode,
  path: RouterNodePath,
  replacement: RouterNode,
): string | null {
  const internalConflict = duplicateConditionInSubtree(replacement);
  if (internalConflict) return internalConflict;
  if (path.length === 0 || replacement.kind !== 'leaf') return null;
  const parentPath = path.slice(0, -1);
  const childIndex = path[path.length - 1];
  const parent = routerNodeAtPath(root, parentPath);
  if (!parent || parent.kind !== 'group') return null;
  return duplicateConditionInChildren(parent.children, replacement, childIndex);
}

export function replaceRouterNodeAtPath(root: RouterNode, path: RouterNodePath, replacement: RouterNode): RouterNode {
  if (path.length === 0) return replacement;
  if (root.kind !== 'group') return root;
  const [index, ...rest] = path;
  if (index < 0 || index >= root.children.length) return root;
  return {
    ...root,
    children: root.children.map((child, childIndex) =>
      childIndex === index ? replaceRouterNodeAtPath(child, rest, replacement) : child
    ),
  };
}

export function removeRouterNodeAtPath(root: RouterNode, path: RouterNodePath): RouterNode {
  if (path.length === 0) return createRouterLeaf();
  if (root.kind !== 'group') return root;
  const [index, ...rest] = path;
  if (index < 0 || index >= root.children.length) return root;

  if (rest.length === 0) {
    if (root.operator === 'not' && root.children.length === 1) {
      return { ...root, children: [] };
    }
    return normalizeRouterNode({
      ...root,
      children: root.children.filter((_, childIndex) => childIndex !== index),
    });
  }

  return normalizeRouterNode({
    ...root,
    children: root.children.map((child, childIndex) =>
      childIndex === index ? removeRouterNodeAtPath(child, rest) : child
    ),
  });
}

export function createEmptyRouterGraphGroup(operator: RouterGroupOperator): RouterGroupNode {
  return {
    id: createRouterNodeId('group'),
    kind: 'group',
    operator,
    children: [],
  };
}

export function appendRouterNodeAtPath(root: RouterNode, targetPath: RouterNodePath, child: RouterNode): RouterNode {
  const target = routerNodeAtPath(root, targetPath);
  if (!target) return root;

  const childConflict = duplicateConditionInSubtree(child);
  if (childConflict) throw new Error(childConflict);

  if (target.kind === 'leaf') {
    const conflict = duplicateConditionInChildren([target], child);
    if (conflict) throw new Error(conflict);
    return replaceRouterNodeAtPath(root, targetPath, {
      id: createRouterNodeId('group'),
      kind: 'group',
      operator: 'all',
      children: [target, child],
    });
  }

  if (target.operator === 'not' && target.children.length >= 1) {
    throw new Error('NOT can contain only one condition.');
  }

  const conflict = duplicateConditionInChildren(target.children, child);
  if (conflict) throw new Error(conflict);

  return replaceRouterNodeAtPath(root, targetPath, {
    ...target,
    children: [...target.children, child],
  });
}

export function wrapRouterNode(root: RouterNode, operator: RouterGroupOperator): RouterNode {
  return {
    id: createRouterNodeId('group'),
    kind: 'group',
    operator,
    children: [root],
  };
}

export function isBlankRouterGraphNode(node: RouterNode): boolean {
  return node.kind === 'leaf'
    && node.type === 'keywords_any'
    && String(node.textValue || '').trim().length === 0;
}
