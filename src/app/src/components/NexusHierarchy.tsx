import React, { useEffect, useId, useMemo, useRef, useState } from 'react';
import { useI18n } from '../i18n';
import { NEXUS_GROUP_PERMISSIONS, type NexusStatus } from '../features/nexus/nexus';
import {
  createNexusGroup, getNexusTreeBranch, renameNexusGroup, saveNexusGroupPermissions,
  nexusKeysEqual, treeBreadcrumbs, visibleTreeRows, type NexusTreeBranches, type NexusTreeEntry,
} from '../features/nexus/tree';
import { Icon } from './Icon';
import { WorkspaceActionButton } from './WorkspacePanels';

export const nexusTreeService = {
  load: getNexusTreeBranch, create: createNexusGroup, rename: renameNexusGroup, permissions: saveNexusGroupPermissions,
};

export function NexusHierarchy({ status, advanced, service = nexusTreeService }: {
  status: NexusStatus;
  advanced: boolean;
  service?: typeof nexusTreeService;
}) {
  const { t } = useI18n();
  const [branches, setBranches] = useState<NexusTreeBranches>({});
  const [rootId, setRootId] = useState('');
  const [selectedId, setSelectedId] = useState('');
  const [expanded, setExpanded] = useState(new Set<string>());
  const [loading, setLoading] = useState(new Set<string>());
  const [errors, setErrors] = useState<Record<string, string>>({});
  const [query, setQuery] = useState('');
  const [notice, setNotice] = useState('');
  const generation = useRef(0);
  const inFlight = useRef(new Set<string>());
  const treeRef = useRef<HTMLDivElement>(null);
  const searchRef = useRef<HTMLInputElement>(null);
  const searchId = useId();

  const load = async (id?: string, reset = false) => {
    const key = id || 'root';
    if (inFlight.current.has(key)) return;
    const request = generation.current;
    inFlight.current.add(key);
    setLoading(current => new Set(current).add(key));
    setErrors(current => { const next = { ...current }; delete next[key]; return next; });
    try {
      const branch = await service.load(id);
      if (request !== generation.current) return;
      setBranches(current => ({ ...(reset ? {} : current), [branch.parent.id]: branch }));
      if (!id) {
        setRootId(branch.parent.id);
        setSelectedId(current => reset || !current ? branch.parent.id : current);
        setExpanded(current => reset ? new Set([branch.parent.id]) : new Set(current).add(branch.parent.id));
      }
    } catch (error) {
      if (request === generation.current) setErrors(current => ({ ...current, [key]: error instanceof Error ? error.message : String(error) }));
    } finally {
      if (request === generation.current) {
        inFlight.current.delete(key);
        setLoading(current => { const next = new Set(current); next.delete(key); return next; });
      }
    }
  };

  useEffect(() => {
    generation.current += 1;
    inFlight.current = new Set();
    setBranches({}); setRootId(''); setSelectedId(''); setExpanded(new Set()); setLoading(new Set()); setErrors({}); setQuery(''); setNotice('');
    void load();
    return () => { generation.current += 1; };
  }, [status.group_node_id, status.node_id, service]);

  const entries = useMemo(() => {
    const index = new Map<string, NexusTreeEntry>();
    Object.values(branches).forEach(branch => branch.children.forEach(child => index.set(child.id, child)));
    Object.values(branches).forEach(branch => index.set(branch.parent.id, branch.parent));
    return index;
  }, [branches]);
  const root = entries.get(rootId);
  const selected = entries.get(selectedId) || root;
  const rows = root ? visibleTreeRows(root, branches, expanded) : [];
  const name = (entry: NexusTreeEntry) => entry.name || (entry.kind === 'group' ? t('Unnamed group') : t('Unnamed device'));
  const select = (entry: NexusTreeEntry) => {
    setSelectedId(entry.id); setNotice('');
    if (entry.kind === 'group' && !branches[entry.id]) void load(entry.id);
  };
  const toggle = (entry: NexusTreeEntry) => {
    setSelectedId(entry.id);
    setExpanded(current => { const next = new Set(current); if (!next.delete(entry.id)) next.add(entry.id); return next; });
    if (!branches[entry.id]) void load(entry.id);
  };
  const focusRow = (id: string) => {
    setSelectedId(id);
    Array.from(treeRef.current?.querySelectorAll<HTMLElement>('[role="treeitem"]') || []).find(element => element.dataset.nodeId === id)?.focus();
  };
  const keyDown = (event: React.KeyboardEvent, entry: NexusTreeEntry, index: number) => {
    let target: string | undefined;
    switch (event.key) {
      case 'ArrowDown': target = rows[Math.min(index + 1, rows.length - 1)]?.entry.id; break;
      case 'ArrowUp': target = rows[Math.max(0, index - 1)]?.entry.id; break;
      case 'Home': target = rows[0]?.entry.id; break;
      case 'End': target = rows.at(-1)?.entry.id; break;
      case 'ArrowRight':
        if (entry.kind === 'group') {
          if (!expanded.has(entry.id)) toggle(entry);
          else target = branches[entry.id]?.children[0]?.id;
        }
        break;
      case 'ArrowLeft':
        if (entry.kind === 'group' && expanded.has(entry.id)) toggle(entry);
        else target = entry.parent_id;
        break;
      case 'Enter': case ' ': select(entry); if (entry.kind === 'group') toggle(entry); break;
      default: return;
    }
    event.preventDefault();
    if (target && entries.has(target)) { select(entries.get(target)!); focusRow(target); }
  };
  const searchResults = query.trim() ? [...entries.values()].filter(entry => name(entry).toLocaleLowerCase().includes(query.trim().toLocaleLowerCase())) : null;
  const compactGroup = selected?.kind === 'group' ? selected : entries.get(selected?.parent_id || '') || root;
  const compactChildren = compactGroup ? branches[compactGroup.id]?.children || [] : [];
  const online = (entry: NexusTreeEntry) => status.members.find(member => member.node_id === entry.id)?.is_online;
  const childCounts = (id: string) => {
    const children = branches[id].children;
    const groups = children.filter(child => child.kind === 'group').length;
    const nodes = children.filter(child => child.kind === 'node').length;
    return <div className="nexus-tree-counts"><span>{groups === 1 ? t('1 group') : t('{count} groups', { count: groups })}</span><span>{nodes === 1 ? t('1 node') : t('{count} nodes', { count: nodes })}</span></div>;
  };

  return <section className="nexus-hierarchy" aria-label={t('Groups and nodes')}>
    <div className="nexus-hierarchy__toolbar">
      <div><strong>{advanced ? t('Groups and nodes') : t('Connected network')}</strong><p>{advanced ? t('Groups can contain groups or device nodes.') : t('Browse your groups and devices in one compact view.')}</p></div>
      <WorkspaceActionButton icon="rotate-ccw" size="small" disabled={loading.size > 0} onClick={() => { generation.current += 1; inFlight.current = new Set(); void load(undefined, true); }}>{t('Refresh')}</WorkspaceActionButton>
    </div>
    {errors.root && <div className="nexus-tree-message" role="alert"><p>{errors.root}</p><WorkspaceActionButton onClick={() => void load()}>{t('Retry')}</WorkspaceActionButton></div>}
    {!root && loading.has('root') && <p className="nexus-tree-message" role="status">{t('Loading groups…')}</p>}
    {root && <>
      <div className="nexus-hierarchy__search">
        <Icon name="search" size={16} /><label className="sr-only" htmlFor={searchId}>{t('Find a group or device')}</label>
        <input ref={searchRef} id={searchId} value={query} placeholder={t('Find a group or device')} onChange={event => setQuery(event.target.value)} />
        {query && <WorkspaceActionButton icon="x" iconOnly size="small" appearance="quiet" aria-label={t('Clear search')} onClick={() => { setQuery(''); searchRef.current?.focus(); }} />}
      </div>
      {searchResults ? <div className="nexus-tree-search-results">
        <p className="nexus-hint">{t('Search covers the groups you have opened.')}</p>
        {searchResults.length ? searchResults.map(entry => <button type="button" key={entry.id} onClick={() => {
          treeBreadcrumbs(entry, entries).forEach(ancestor => setExpanded(current => new Set(current).add(ancestor.id)));
          setQuery(''); select(entry); searchRef.current?.focus();
        }}><Icon name={entry.kind === 'group' ? 'layers' : 'monitor'} size={18} /><span>{name(entry)}<small>{treeBreadcrumbs(entry, entries).map(name).join(' / ')}</small></span><Icon name="chevron-right" size={14} /></button>) : <p>{t('No matching groups or devices.')}</p>}
      </div> : advanced ? <div className="nexus-hierarchy__workspace">
        <div className="nexus-tree" ref={treeRef} role="tree" aria-label={t('Group hierarchy')}>
          {rows.map(({ entry, depth, position, siblings }, index) => <div key={entry.id} role="treeitem" tabIndex={selected?.id === entry.id ? 0 : -1}
            data-node-id={entry.id} aria-label={name(entry)} aria-level={depth} aria-posinset={position} aria-setsize={siblings}
            aria-selected={selected?.id === entry.id} aria-expanded={entry.kind === 'group' ? expanded.has(entry.id) : undefined}
            className={`nexus-tree__row${selected?.id === entry.id ? ' is-selected' : ''}`} style={{ '--tree-depth': Math.min(depth - 1, 8) } as React.CSSProperties}
            onClick={() => select(entry)} onKeyDown={event => keyDown(event, entry, index)}>
            <span className="nexus-tree__disclosure" aria-hidden="true" onClick={event => { if (entry.kind === 'group') { event.stopPropagation(); toggle(entry); } }}>
              {entry.kind === 'group' && <Icon name={expanded.has(entry.id) ? 'chevron-down' : 'chevron-right'} size={14} />}
            </span>
            <Icon name={entry.kind === 'group' ? 'layers' : 'monitor'} size={17} />
            <span className="nexus-tree__name">{name(entry)}</span>
            {loading.has(entry.id) && <span className="nexus-hint">{t('Loading…')}</span>}
            {entry.id === status.node_id && <span className="nexus-tree__you">{t('This device')}</span>}
          </div>)}
        </div>
        {selected && <div className="nexus-tree-detail">
          <nav className="nexus-tree-breadcrumbs" aria-label={t('Group path')}>{treeBreadcrumbs(selected, entries).map((entry, index) => <React.Fragment key={entry.id}>
            {index > 0 && <Icon name="chevron-right" size={12} />}<button type="button" onClick={() => select(entry)} aria-current={entry.id === selected.id ? 'location' : undefined}>{name(entry)}</button>
          </React.Fragment>)}</nav>
          <div className="nexus-tree-detail__heading"><span className="nexus-tree-detail__icon"><Icon name={selected.kind === 'group' ? 'layers' : 'monitor'} size={23} /></span><div><h4>{name(selected)}</h4><p>{selected.kind === 'group' ? t('Group') : selected.is_inference ? t('Inference server') : t('Device node')}</p></div></div>
          {selected.kind === 'group' ? <>
            <p className="nexus-hint">{t('Organize by person, team or location. Nest groups as your network grows.')}</p>
            {loading.has(selected.id) && <p role="status">{t('Loading group…')}</p>}
            {errors[selected.id] && <div role="alert"><p className="nexus-error">{errors[selected.id]}</p><WorkspaceActionButton size="small" onClick={() => void load(selected.id)}>{t('Retry')}</WorkspaceActionButton></div>}
            {branches[selected.id] && childCounts(selected.id)}
            <GroupManagement key={selected.id} entry={selected} entries={entries} selfId={status.node_id} service={service} onSaved={async message => {
              await load(selected.id); if (branches[selected.parent_id]) await load(selected.parent_id);
              setExpanded(current => new Set(current).add(selected.id)); setNotice(message);
            }} />
          </> : <dl className="nexus-tree-facts"><div><dt>{t('Status')}</dt><dd>{online(selected) === undefined ? t('Unknown') : online(selected) ? t('Online') : t('Offline')}</dd></div><div><dt>{t('Mesh address')}</dt><dd>{selected.tunnel_ip || t('Not connected')}</dd></div></dl>}
          <details className="nexus-tree-identifiers"><summary>{t('Technical details')}</summary><dl className="nexus-tree-facts"><div><dt>{t('Node ID')}</dt><dd>{selected.id}</dd></div><div><dt>{t('Parent group')}</dt><dd>{selected.parent_id || '—'}</dd></div></dl></details>
          {notice && <p className="nexus-tree-notice" role="status">{notice}</p>}
        </div>}
      </div> : <div className="nexus-compact">
        {compactGroup && <nav className="nexus-tree-breadcrumbs" aria-label={t('Group path')}>{treeBreadcrumbs(compactGroup, entries).map(entry => <button key={entry.id} type="button" onClick={() => select(entry)} aria-current={entry.id === compactGroup.id ? 'location' : undefined}>{name(entry)}</button>)}</nav>}
        {compactGroup && errors[compactGroup.id] && <p className="nexus-error" role="alert">{errors[compactGroup.id]} <button type="button" onClick={() => void load(compactGroup.id)}>{t('Retry')}</button></p>}
        {compactGroup && loading.has(compactGroup.id) ? <p role="status">{t('Loading group…')}</p> : compactChildren.length ? compactChildren.map(entry => <button key={entry.id} type="button" className="nexus-compact__row" disabled={entry.kind === 'node'} onClick={() => select(entry)}>
          <Icon name={entry.kind === 'group' ? 'layers' : 'monitor'} size={21} /><span><strong>{name(entry)}</strong><small>{entry.kind === 'group' ? t('Group') : entry.id === status.node_id ? t('This device') : online(entry) === undefined ? t('Status unknown') : online(entry) ? t('Online') : t('Offline')}</small></span>{entry.kind === 'group' && <Icon name="chevron-right" size={16} />}
        </button>) : <p className="nexus-tree-message">{compactGroup?.can_add_group ? t('This group is empty. Use Advanced to add a group.') : t('No groups or devices here yet.')}</p>}
      </div>}
    </>}
  </section>;
}

function GroupManagement({ entry, entries, selfId, service, onSaved }: {
  entry: NexusTreeEntry; entries: ReadonlyMap<string, NexusTreeEntry>; selfId?: string;
  service: typeof nexusTreeService; onSaved: (message: string) => Promise<void>;
}) {
  const { t } = useI18n();
  const id = useId();
  const [editor, setEditor] = useState<'create' | 'rename' | 'permissions'>();
  const [name, setName] = useState('');
  const [memberId, setMemberId] = useState('');
  const [admin, setAdmin] = useState(false);
  const [permissions, setPermissions] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const actionRef = useRef<HTMLButtonElement | null>(null);
  useEffect(() => { if (!editor && !busy) actionRef.current?.focus(); }, [editor, busy]);
  const members = [...entries.values()].filter(node => node.kind === 'node' && node.node_type === 'endpoint' && node.id !== selfId);
  const chooseMember = (value: string) => {
    setMemberId(value);
    const member = entries.get(value);
    const assignment = entry.assignments.find(item => nexusKeysEqual(item.management_pubkey, member?.management_pubkey || ''));
    setAdmin(Boolean(assignment?.permissions.includes('admin')));
    setPermissions(assignment?.permissions.filter(permission => permission !== 'admin') || ['read']);
  };
  const canEdit = editor === 'create' ? entry.can_add_group : editor === 'rename' ? entry.can_rename : entry.can_manage;
  const nameTooLong = new TextEncoder().encode(name.trim()).length > 128;
  const run = async (event: React.FormEvent) => {
    event.preventDefault();
    if (busy || !canEdit) return;
    setBusy(true); setError('');
    try {
      if (editor === 'create') await service.create(entry.id, name.trim());
      else if (editor === 'rename') await service.rename(entry.id, name.trim());
      else await service.permissions(entry.id, memberId, admin, permissions);
      await onSaved(editor === 'create' ? t('Group created.') : editor === 'rename' ? t('Group renamed.') : t('Permissions saved.'));
      setEditor(undefined);
    } catch (error) {
      setError(error instanceof TypeError || (error instanceof DOMException && (error.name === 'TimeoutError' || error.name === 'AbortError'))
        ? t('The connection was interrupted. Refresh the group to check whether the change was saved before trying again.')
        : error instanceof Error ? error.message : String(error));
    }
    finally { setBusy(false); }
  };
  return <div className="nexus-group-management">
    <div className="nexus-group-actions">
      {entry.can_add_group && <WorkspaceActionButton icon="plus" size="small" disabled={busy} onClick={event => { actionRef.current = event.currentTarget; setName(''); setError(''); setEditor('create'); }}>{t('Add group')}</WorkspaceActionButton>}
      {entry.can_rename && <WorkspaceActionButton icon="edit" size="small" appearance="quiet" disabled={busy} onClick={event => { actionRef.current = event.currentTarget; setName(entry.name); setError(''); setEditor('rename'); }}>{t('Rename')}</WorkspaceActionButton>}
      {entry.can_manage && <WorkspaceActionButton icon="user-round-cog" size="small" appearance="quiet" disabled={busy} onClick={event => { actionRef.current = event.currentTarget; chooseMember(''); setError(''); setEditor('permissions'); }}>{t('Permissions')}</WorkspaceActionButton>}
    </div>
    {!entry.can_add_group && !entry.can_rename && !entry.can_manage && <p className="nexus-hint">{t('You have read-only access to this group.')}</p>}
    {editor && <form className="nexus-group-editor" onSubmit={event => void run(event)}>
      <h5>{editor === 'create' ? t('Add a group inside {name}', { name: entry.name || t('Unnamed group') }) : editor === 'rename' ? t('Rename group') : t('Group permissions')}</h5>
      <fieldset disabled={busy || !canEdit}>
        {editor !== 'permissions' ? <><label htmlFor={`${id}-name`}>{t('Group name')}</label><input autoFocus id={`${id}-name`} className="input" value={name} maxLength={128} onChange={event => setName(event.target.value)} placeholder={t('For example, Design team')} /></> : <>
          <label htmlFor={`${id}-member`}>{t('Device identity')}</label>
          <select id={`${id}-member`} className="input" value={memberId} onChange={event => chooseMember(event.target.value)}><option value="">{t('Choose a device')}</option>{members.map(member => <option key={member.id} value={member.id}>{member.name || member.id}</option>)}</select>
          <p className="nexus-hint">{t('Choose from discovered devices. Open other groups to find more. These permissions belong to devices, not user accounts.')}</p>
          {memberId && <>
            <label className="nexus-permission-choice"><input type="checkbox" checked={admin} onChange={event => setAdmin(event.target.checked)} />{t('Group administrator')}</label>
            <p className="nexus-hint">{admin ? t('Administrators can manage this group and its descendants.') : t('Grant only the access this device needs.')}</p>
            <div className="nexus-permissions">{NEXUS_GROUP_PERMISSIONS.map(permission => <label key={permission.name}><input type="checkbox" checked={admin || permission.name === 'read' || permissions.includes(permission.name)} disabled={admin || permission.name === 'read'} onChange={event => setPermissions(current => event.target.checked ? [...current, permission.name] : current.filter(value => value !== permission.name))} />{t(permission.label)}</label>)}</div>
          </>}
        </>}
      </fieldset>
      {error && <p className="nexus-error" role="alert">{error}</p>}
      {editor !== 'permissions' && nameTooLong && <p className="nexus-error" role="alert">{t('This name is too long. Try a shorter name.')}</p>}
      {!canEdit && <p className="nexus-error" role="alert">{t('Your access has changed. Refresh before editing.')}</p>}
      <div className="nexus-group-actions"><WorkspaceActionButton type="submit" disabled={busy || !canEdit || (editor === 'permissions' ? !memberId : !name.trim() || nameTooLong)}>{busy ? t('Saving…') : editor === 'create' ? t('Create group') : t('Save changes')}</WorkspaceActionButton><WorkspaceActionButton appearance="quiet" disabled={busy} onClick={() => { setEditor(undefined); actionRef.current?.focus(); }}>{t('Cancel')}</WorkspaceActionButton></div>
    </form>}
  </div>;
}
