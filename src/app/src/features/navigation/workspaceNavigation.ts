import type { IconName } from '../../components/Icon';

function defineSection<Id extends string>(
  id: Id,
  icon: IconName,
) {
  return {
    id,
    labelKey: `sections.${id}.label`,
    descriptionKey: `sections.${id}.description`,
    icon,
  } as const;
}

type DefinedSection = ReturnType<typeof defineSection>;

function defineWorkspace<
  Id extends string,
  Sections extends readonly DefinedSection[],
>(id: Id, sections: Sections): {
  id: Id;
  labelKey: `workspaces.${Id}.label`;
  defaultSection: Sections[0]['id'];
  sections: Sections;
} {
  return {
    id,
    labelKey: `workspaces.${id}.label`,
    defaultSection: sections[0].id as Sections[0]['id'],
    sections,
  };
}

export const WORKSPACE_NAVIGATION = {
  dashboard: defineWorkspace('dashboard', [
    defineSection('performance', 'gauge'),
    defineSection('telemetry', 'search-check'),
    defineSection('logs', 'logs'),
  ] as const),
  connect: defineWorkspace('connect', [
    defineSection('server', 'plug'),
    defineSection('chat', 'chat'),
    defineSection('memory', 'gauge'),
    defineSection('language', 'globe'),
    defineSection('model-storage', 'hard-drive'),
    defineSection('cloud-providers', 'cloud'),
    defineSection('mcp-gateway', 'tools'),
    defineSection('help-and-support', 'book-open'),
  ] as const),
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
