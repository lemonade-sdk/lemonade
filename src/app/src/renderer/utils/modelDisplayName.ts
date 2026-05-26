// Canonical-prefix table for shadowed-source model IDs. A "shadowed" id is
// emitted by the server with one of these prefixes when a higher-precedence
// source already owns the bare name. Winners are emitted with no prefix and
// render as the bare name.
export const CANONICAL_PREFIXES: { prefix: string; suffix: string; sourceRank: number }[] = [
  { prefix: 'user.',    suffix: ' (registered)', sourceRank: 1 },
  { prefix: 'extra.',   suffix: ' (imported)', sourceRank: 2 },
  { prefix: 'builtin.', suffix: ' (builtin)', sourceRank: 3 },
];

// Render a model id as a human-readable display name. Bare ids (winners) render
// as-is; canonical-prefixed ids (shadowed sources) render as "NAME (source)".
export const getModelDisplayName = (modelName: string): string => {
  const match = CANONICAL_PREFIXES.find(p => modelName.startsWith(p.prefix));
  return match ? modelName.slice(match.prefix.length) + match.suffix : modelName;
};
