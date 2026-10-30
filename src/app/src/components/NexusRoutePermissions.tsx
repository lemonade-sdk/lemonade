import React, { useEffect, useId, useState } from 'react';
import { NEXUS_SERVICE_PERMISSIONS } from '../features/nexus/nexus';
import { getNexusRoutePermissions, type NexusRole, type NexusRoutePermissions as RoutePermissions } from '../features/nexus/routePermissions';
import { useI18n } from '../i18n';
import { Icon } from './Icon';
import { WorkspaceActionButton } from './WorkspacePanels';

const roleLabels: Record<NexusRole, string> = { admin: 'Admin', moderator: 'Moderator', user: 'User' };

export function NexusRoutePermissions() {
  const { t } = useI18n();
  const id = useId();
  const [open, setOpen] = useState(false);
  const [role, setRole] = useState<NexusRole>('user');
  const [query, setQuery] = useState('');
  const [data, setData] = useState<RoutePermissions>();
  const [error, setError] = useState('');
  const [attempt, setAttempt] = useState(0);
  useEffect(() => {
    if (!open) return;
    let active = true;
    setData(undefined); setError('');
    void getNexusRoutePermissions().then(value => {
      if (active) setData(value);
    }).catch(error => {
      if (active) setError(error instanceof Error ? error.message : String(error));
    });
    return () => { active = false; };
  }, [open, attempt]);
  const familyLabel = (family: string) => t(NEXUS_SERVICE_PERMISSIONS.find(permission => permission.name === `svc:${family}`)?.label || family);
  const allowed = data?.routes.filter(route => route.roles.includes(role)) || [];
  const search = query.trim().toLocaleLowerCase();
  const filtered = allowed.filter(route => `${route.method} ${route.paths.join(' ')} ${familyLabel(route.family)}`.toLocaleLowerCase().includes(search));
  const families = [...new Set(filtered.map(route => route.family))];

  return <details className="nexus-route-permissions" onToggle={event => setOpen(event.currentTarget.open)}>
    <summary><Icon name="chevron-right" size={16} /><span><strong>{t('Route permissions')}</strong><small>{t('Allowed mesh routes by role')}</small></span></summary>
    {open && <div className="nexus-route-permissions__content">
      {error ? <div role="alert"><p className="nexus-error">{t(error)}</p><WorkspaceActionButton size="small" onClick={() => setAttempt(value => value + 1)}>{t('Retry')}</WorkspaceActionButton></div>
        : !data ? <p className="nexus-hint" role="status">{t('Loading route permissions…')}</p>
        : data.route_policy !== 'enforced' ? <div role="alert"><p className="nexus-hint">{t('The server cannot verify its route policy. Refresh after the server is ready.')}</p><WorkspaceActionButton size="small" onClick={() => setAttempt(value => value + 1)}>{t('Retry')}</WorkspaceActionButton></div>
        : <>
          <p className="nexus-hint">{t('Preset access through the authenticated mesh. Device and group assignments can limit access.')}</p>
          <div className="nexus-route-permissions__toolbar">
            <fieldset className="nexus-view-mode nexus-role-selector"><legend className="sr-only">{t('View routes for role')}</legend>
              {data.roles.map(item => <label key={item.id} className={role === item.id ? 'is-selected' : ''}>
                <input type="radio" name={id} checked={role === item.id} onChange={() => setRole(item.id)} />
                {t(roleLabels[item.id])}<span className="nexus-route-count" aria-hidden="true">{data.routes.filter(route => route.roles.includes(item.id)).length}</span>
              </label>)}
            </fieldset>
            <label className="nexus-route-search"><Icon name="search" size={16} /><span className="sr-only">{t('Filter routes')}</span><input type="search" value={query} placeholder={t('Filter routes')} onChange={event => setQuery(event.target.value)} /></label>
          </div>
          <p className="nexus-route-permissions__count" role="status">{t('{count} allowed routes', { count: filtered.length })}</p>
          <div className="nexus-route-permissions__list">
            {families.map(family => {
              const routes = filtered.filter(route => route.family === family);
              return <details key={`${role}:${family}:${search}`} className="nexus-route-family" open={Boolean(search)}>
                <summary><Icon name="chevron-right" size={14} /><span>{familyLabel(family)}</span><span className="nexus-route-count">{routes.length}</span></summary>
                {family === 'management' && <p className="nexus-hint">{t('Nexus administration requires Admin. Group actions also check current group assignments.')}</p>}
                <table><caption className="sr-only">{t('{role}: {service}', { role: t(roleLabels[role]), service: familyLabel(family) })}</caption>
                  <thead><tr><th scope="col">{t('Method')}</th><th scope="col">{t('Allowed route')}</th></tr></thead>
                  <tbody>{routes.map(route => {
                    const primary = route.paths.find(path => path.startsWith('/api/v1/')) || route.paths[0];
                    const aliases = route.paths.filter(path => path !== primary);
                    return <tr key={`${route.method}:${primary}`}><td><span className="nexus-route-method">{route.method}</span></td><td><code>{primary}</code>
                      {aliases.length > 0 && <details className="nexus-route-aliases"><summary>{t('{count} aliases', { count: aliases.length })}</summary>{aliases.map(path => <code key={path}>{path}</code>)}</details>}
                    </td></tr>;
                  })}</tbody>
                </table>
              </details>;
            })}
            {!families.length && <p className="nexus-hint">{search ? t('No routes match your search.') : t('This role has no allowed mesh routes.')}</p>}
          </div>
          <p className="nexus-hint nexus-route-permissions__scope">{t('Local setup, identity operations and internal endpoints are excluded. Viewing a role does not change device permissions.')}</p>
        </>}
    </div>}
  </details>;
}
