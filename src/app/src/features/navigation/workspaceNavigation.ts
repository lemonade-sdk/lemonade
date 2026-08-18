import type { IconName } from '../../components/Icon';

const ROUTE_ACRONYMS: Record<string, string> = {
  mcp: 'MCP',
};

/* Sentence case throughout: only the first word and known acronyms are
 * capitalized, so "Model storage" and "MCP gateway" read as one set. */
export function workspaceRouteLabel(routeSegment: string): string {
  const parts = routeSegment.split('-').filter(Boolean);
  return parts
    .map((part, index) => {
      if (part === 'and') return '&';
      if (ROUTE_ACRONYMS[part]) return ROUTE_ACRONYMS[part];
      return index === 0 ? `${part.charAt(0).toUpperCase()}${part.slice(1)}` : part;
    })
    .join(' ');
}

function defineSection<Id extends string>(
  id: Id,
  description: string,
  icon: IconName,
) {
  return { id, label: workspaceRouteLabel(id), description, icon };
}

type DefinedSection = ReturnType<typeof defineSection>;

function defineWorkspace<
  Id extends string,
  Sections extends readonly DefinedSection[],
>(id: Id, sections: Sections, label?: string): {
  id: Id;
  label: string;
  defaultSection: Sections[0]['id'];
  sections: Sections;
} {
  return {
    id,
    label: label ?? workspaceRouteLabel(id),
    defaultSection: sections[0].id as Sections[0]['id'],
    sections,
  };
}

export const WORKSPACE_NAVIGATION = {
  dashboard: defineWorkspace('dashboard', [
    defineSection('performance', 'Health and throughput', 'gauge'),
    defineSection('telemetry', 'Traces, replay and tuning', 'search-check'),
    defineSection('logs', 'Live server output', 'logs'),
  ] as const, 'Monitor'),
  connect: defineWorkspace('connect', [
    defineSection('server', 'Endpoint and authentication', 'plug'),
    defineSection('chat', 'History, reasoning, and speech', 'chat'),
    defineSection('memory', 'Budget, Loading and eviction', 'gauge'),
    defineSection('model-storage', 'Cache and custom directories', 'hard-drive'),
    defineSection('cloud-providers', 'OpenAI-compatible services', 'cloud'),
    defineSection('mcp-gateway', 'Tools and external servers', 'tools'),
    defineSection('help-and-support', 'Docs, releases and community', 'book-open'),
  ] as const, 'Settings'),
} as const;

export type RoutedWorkspace = keyof typeof WORKSPACE_NAVIGATION;
export type DashboardSection = typeof WORKSPACE_NAVIGATION.dashboard.sections[number]['id'];
export type ConnectSection = typeof WORKSPACE_NAVIGATION.connect.sections[number]['id'];

export type WorkspaceRoute =
  | { workspace: 'dashboard'; section: DashboardSection }
  | { workspace: 'connect'; section: ConnectSection };

export function isRoutedWorkspace(value: string): value is RoutedWorkspace {
  return value in WORKSPACE_NAVIGATION;
}

export function workspaceRouteFromPath(path: string): WorkspaceRoute | null {
  const normalizedPath = path.trim().replace(/^\/+|\/+$/g, '').toLowerCase();
  const [workspaceValue, sectionValue, ...rest] = normalizedPath.split('/');
  if (rest.length > 0 || !isRoutedWorkspace(workspaceValue)) return null;

  const definition = WORKSPACE_NAVIGATION[workspaceValue];
  const section = sectionValue || definition.defaultSection;
  if (!definition.sections.some(item => item.id === section)) return null;
  return { workspace: workspaceValue, section } as WorkspaceRoute;
}

export function workspaceHash(route: WorkspaceRoute): string {
  return `#/${route.workspace}/${route.section}`;
}
