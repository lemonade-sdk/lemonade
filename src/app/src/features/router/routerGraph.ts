import {
  createRouterLeaf,
  createRouterNodeId,
  normalizeRouterNode,
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

  if (target.kind === 'leaf') {
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
