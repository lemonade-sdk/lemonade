import React from 'react';
import { CapabilityIcon, Icon, type CapabilityIconTarget, type IconName } from './Icon';
import { CAPABILITY_TAG_LABELS, type CapabilityTag } from '../modelCapabilities';
import { capabilityColor } from '../modelPresentation';

export type WorkspaceMetadataEmphasis = 'high' | 'medium' | 'low';
export type WorkspaceMetadataTone = 'neutral' | 'accent' | 'success' | 'warning';

interface WorkspaceMetadataChipProps {
  children: React.ReactNode;
  emphasis?: WorkspaceMetadataEmphasis;
  tone?: WorkspaceMetadataTone;
  icon?: IconName;
  className?: string;
  href?: string;
  target?: string;
  rel?: string;
  title?: string;
  buttonProps?: React.ButtonHTMLAttributes<HTMLButtonElement>;
  dataAttributes?: Record<string, string | boolean>;
}

export const WorkspaceMetadataChip: React.FC<WorkspaceMetadataChipProps> = ({
  children,
  emphasis = 'low',
  tone = 'neutral',
  icon,
  className = '',
  href,
  target,
  rel,
  title,
  buttonProps,
  dataAttributes,
}) => {
  const chipClassName = `workspace-metadata-chip workspace-metadata-chip--${emphasis} workspace-metadata-chip--${tone}${className ? ` ${className}` : ''}`;
  const content = <>{icon && <Icon name={icon} size={12} aria-hidden="true" />}{children}</>;

  if (href) {
    return <a {...dataAttributes} className={chipClassName} href={href} target={target} rel={rel} title={title}>{content}</a>;
  }

  if (buttonProps) {
    return <button {...buttonProps} {...dataAttributes} type="button" className={`${chipClassName}${buttonProps.className ? ` ${buttonProps.className}` : ''}`} title={title ?? buttonProps.title}>{content}</button>;
  }

  return <span {...dataAttributes} className={chipClassName} title={title}>{content}</span>;
};

const METADATA_EMPHASIS_RANK: Record<WorkspaceMetadataEmphasis, number> = {
  high: 0,
  medium: 1,
  low: 2,
};

function flattenedMetadataChildren(children: React.ReactNode, parentKey = 'metadata'): React.ReactNode[] {
  return React.Children.toArray(children).flatMap((child, index) => {
    const childKey = React.isValidElement(child) && child.key !== null ? String(child.key) : String(index);
    const key = `${parentKey}/${childKey}`;
    if (React.isValidElement<{ children?: React.ReactNode }>(child) && child.type === React.Fragment) {
      return flattenedMetadataChildren(child.props.children, key);
    }
    return [React.isValidElement(child) ? React.cloneElement(child, { key }) : child];
  });
}

export const WorkspaceMetadataGroup: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const orderedChildren = React.useMemo(() => flattenedMetadataChildren(children)
    .map((child, index) => ({ child, index }))
    .sort((left, right) => {
      const leftEmphasis = React.isValidElement<WorkspaceMetadataChipProps>(left.child)
        ? left.child.props.emphasis ?? 'low'
        : 'low';
      const rightEmphasis = React.isValidElement<WorkspaceMetadataChipProps>(right.child)
        ? right.child.props.emphasis ?? 'low'
        : 'low';
      return METADATA_EMPHASIS_RANK[leftEmphasis] - METADATA_EMPHASIS_RANK[rightEmphasis]
        || left.index - right.index;
    })
    .map(({ child }) => child), [children]);

  return <div className="workspace-detail-panel__metadata">{orderedChildren}</div>;
};

export type WorkspaceActionAppearance = 'primary' | 'secondary' | 'quiet' | 'danger';
export type WorkspaceActionSize = 'small' | 'medium' | 'toolbar';

function workspaceActionClassName(
  appearance: WorkspaceActionAppearance,
  size: WorkspaceActionSize,
  iconOnly: boolean,
  className: string,
): string {
  return `btn btn--${appearance} btn--${size} workspace-action-button workspace-action-button--${appearance} workspace-action-button--${size}${iconOnly ? ' btn--icon-only workspace-action-button--icon-only' : ''}${className ? ` ${className}` : ''}`;
}

interface WorkspaceActionBase {
  appearance?: WorkspaceActionAppearance;
  size?: WorkspaceActionSize;
  icon?: IconName;
}

/* An icon-only control drops its children, so the accessible name has to come
 * from somewhere else. Requiring aria-label here makes that structural instead
 * of a review checklist item. */
type WorkspaceActionLabelling =
  | { iconOnly: true; 'aria-label': string }
  | { iconOnly?: false };

type WorkspaceActionButtonProps = React.ButtonHTMLAttributes<HTMLButtonElement>
  & WorkspaceActionBase
  & WorkspaceActionLabelling;

export const WorkspaceActionButton = React.forwardRef<HTMLButtonElement, WorkspaceActionButtonProps>(({
  appearance = 'secondary',
  size = 'medium',
  icon,
  iconOnly = false,
  className = '',
  children,
  type = 'button',
  ...buttonProps
}, ref) => (
  <button
    {...buttonProps}
    ref={ref}
    type={type}
    className={workspaceActionClassName(appearance, size, iconOnly, className)}
  >
    {icon && <Icon name={icon} size={size === 'toolbar' ? 16 : 14} aria-hidden="true" />}
    {!iconOnly && <span className="workspace-action-button__label">{children}</span>}
  </button>
));

WorkspaceActionButton.displayName = 'WorkspaceActionButton';

type WorkspaceActionLinkProps = React.AnchorHTMLAttributes<HTMLAnchorElement>
  & WorkspaceActionBase
  & WorkspaceActionLabelling;

export const WorkspaceActionLink: React.FC<WorkspaceActionLinkProps> = ({
  appearance = 'secondary',
  size = 'medium',
  icon,
  iconOnly = false,
  className = '',
  children,
  ...linkProps
}) => (
  <a
    {...linkProps}
    className={workspaceActionClassName(appearance, size, iconOnly, className)}
  >
    {icon && <Icon name={icon} size={size === 'toolbar' ? 16 : 14} aria-hidden="true" />}
    {!iconOnly && <span className="workspace-action-button__label">{children}</span>}
  </a>
);

interface WorkspaceActionGroupProps {
  children: React.ReactNode;
  className?: string;
  label?: string;
}

export const WorkspaceActionGroup: React.FC<WorkspaceActionGroupProps> = ({ children, className = '', label }) => (
  <div className={`workspace-action-group${className ? ` ${className}` : ''}`} role="group" aria-label={label}>
    {children}
  </div>
);

interface WorkspacePanelResizerProps {
  label: string;
  minWidth: number;
  maxWidth: number;
  value: number;
  onPointerDown: React.PointerEventHandler<HTMLDivElement>;
  onKeyDown: React.KeyboardEventHandler<HTMLDivElement>;
}

export const WorkspacePanelResizer: React.FC<WorkspacePanelResizerProps> = ({
  label,
  minWidth,
  maxWidth,
  value,
  onPointerDown,
  onKeyDown,
}) => (
  <div
    className="workspace-panel-resizer"
    role="separator"
    aria-orientation="vertical"
    aria-label={label}
    aria-valuemin={minWidth}
    aria-valuemax={maxWidth}
    aria-valuenow={value}
    tabIndex={0}
    onPointerDown={onPointerDown}
    onKeyDown={onKeyDown}
  />
);

interface WorkspaceResourceListProps {
  children: React.ReactNode;
  label: string;
  className?: string;
}

export const WorkspaceResourceList: React.FC<WorkspaceResourceListProps> = ({ children, label, className = '' }) => (
  <div className={`workspace-resource-list${className ? ` ${className}` : ''}`} aria-label={label}>
    {children}
  </div>
);

interface WorkspaceResourceRowProps {
  title: React.ReactNode;
  description?: React.ReactNode;
  metadata?: React.ReactNode;
  leading?: React.ReactNode;
  actions?: React.ReactNode;
  onClick?: React.MouseEventHandler<HTMLButtonElement>;
  className?: string;
  ariaLabel?: string;
  ariaDescribedBy?: string;
}

export const WorkspaceResourceRow: React.FC<WorkspaceResourceRowProps> = ({
  title,
  description,
  metadata,
  leading,
  actions,
  onClick,
  className = '',
  ariaLabel,
  ariaDescribedBy,
}) => {
  const content = <>
    {leading && <span className="workspace-resource-row__leading" aria-hidden="true">{leading}</span>}
    <span className="workspace-resource-row__body">
      <strong className="workspace-resource-row__title">{title}</strong>
      {description && <span className="workspace-resource-row__description">{description}</span>}
      {metadata && <span className="workspace-resource-row__metadata">{metadata}</span>}
    </span>
    {actions
      ? <span className="workspace-resource-row__actions">{actions}</span>
      : onClick && <Icon name="chevron-right" size={14} className="workspace-resource-row__chevron" aria-hidden="true" />}
  </>;
  const rowClassName = `workspace-resource-row${onClick ? ' workspace-resource-row--interactive' : ''}${className ? ` ${className}` : ''}`;

  if (onClick) {
    return <button type="button" className={rowClassName} onClick={onClick} aria-label={ariaLabel} aria-describedby={ariaDescribedBy}>{content}</button>;
  }

  return <article className={rowClassName}>{content}</article>;
};

interface WorkspacePaneHeaderProps {
  title: React.ReactNode;
  subtitle?: React.ReactNode;
  headingLevel?: 1 | 2;
  titleId?: string;
  icon?: IconName;
  actions?: React.ReactNode;
  className?: string;
}

export const WorkspacePaneHeader: React.FC<WorkspacePaneHeaderProps> = ({
  title,
  subtitle,
  headingLevel = 2,
  titleId,
  icon,
  actions,
  className = '',
}) => {
  const Heading = headingLevel === 1 ? 'h1' : 'h2';
  return (
    <header className={`workspace-pane__header${className ? ` ${className}` : ''}`}>
      <div className="workspace-pane__heading">
        <Heading id={titleId}>{title}</Heading>
        {subtitle && <p>{subtitle}</p>}
      </div>
      {(actions || icon) && (
        <div className="workspace-pane__header-actions">
          {actions}
          {icon && <span className="workspace-pane__header-icon" aria-hidden="true"><Icon name={icon} size={18} /></span>}
        </div>
      )}
    </header>
  );
};

interface WorkspaceListPanelProps {
  title: string;
  subtitle?: React.ReactNode;
  actions?: React.ReactNode;
  className?: string;
  headerClassName?: string;
  children: React.ReactNode;
}

export const WorkspaceListPanel: React.FC<WorkspaceListPanelProps> = ({
  title,
  subtitle,
  actions,
  className = '',
  headerClassName = '',
  children,
}) => (
  <section className={`workspace-list-panel${className ? ` ${className}` : ''}`} aria-label={title}>
    <header className={`workspace-list-panel__header workspace-pane__header${headerClassName ? ` ${headerClassName}` : ''}`}>
      <div className="workspace-list-panel__heading">
        <h1>{title}</h1>
        {subtitle && <span className="workspace-list-panel__subtitle">{subtitle}</span>}
      </div>
      {actions && <div className="workspace-list-panel__actions">{actions}</div>}
    </header>
    {children}
  </section>
);

/* ── Shared selection list ────────────────────────────────────────
   One row template behind every selectable list in the app: the model
   catalog, remote registry results, chat history, and captured requests.
   DESIGN.md treats a per-tab row variant as prohibited variation. */

/** Operational state. `absent` is the hollow ring; omit entirely when a row
    genuinely has no state, so the rows that do have one stay findable. */
export type WorkspaceListRowStatus =
  | 'ready' | 'live' | 'busy' | 'attention' | 'error' | 'absent' | 'unknown';

/** A non-nominal state is spelled out in the meta line, so the tone follows
    from the state itself rather than being tracked alongside it. */
type WorkspaceListRowTone = 'default' | 'attention' | 'error' | 'busy';

function rowTone(status?: WorkspaceListRowStatus): WorkspaceListRowTone {
  switch (status) {
    case 'busy': return 'busy';
    case 'attention': return 'attention';
    case 'error': return 'error';
    default: return 'default';
  }
}

export interface WorkspaceListRowAction {
  icon: IconName;
  label: string;
  onClick: () => void;
  /** Latched actions (a pinned row) stay visible instead of waiting for hover. */
  active?: boolean;
}

interface WorkspaceListProps {
  children: React.ReactNode;
  label: string;
  className?: string;
  listRef?: React.RefObject<HTMLUListElement | null>;
  tabIndex?: number;
  /** Roving focus landed on a row, identified by its `rowId`. */
  onRowFocus?: (rowId: string) => void;
  /** Enter/Space on a row — and every arrow move when `activateOnMove`. */
  onRowActivate?: (rowId: string) => void;
  /** Arrow keys wrap past the ends instead of stopping there. */
  wrap?: boolean;
  /** Single-select listbox: moving focus also selects (ARIA APG). */
  activateOnMove?: boolean;
}

/**
 * Owns the ARIA listbox roving-tabindex behaviour for every selection list.
 * Rows are found by `role="option"` and identified by `data-row-id`, so a list
 * only supplies policy (`wrap`, `activateOnMove`) and its two callbacks.
 */
export const WorkspaceList: React.FC<WorkspaceListProps> = ({
  children, label, className = '', listRef, tabIndex,
  onRowFocus, onRowActivate, wrap = false, activateOnMove = false,
}) => {
  const handleKeyDown = (event: React.KeyboardEvent<HTMLUListElement>) => {
    // A row may claim a key first — the catalog's pin shortcut does.
    if (event.defaultPrevented) return;

    const options = Array.from(
      event.currentTarget.querySelectorAll<HTMLElement>('[role="option"]'),
    );
    if (!options.length) return;

    // Focus may sit on the row's action button rather than the row itself.
    const current = options.findIndex(option =>
      option === document.activeElement || option.contains(document.activeElement),
    );
    const last = options.length - 1;
    const step = (delta: number) => {
      if (current < 0) return delta > 0 ? 0 : (wrap ? last : 0);
      const moved = current + delta;
      if (wrap) return (moved + options.length) % options.length;
      return Math.min(Math.max(moved, 0), last);
    };

    let next = -1;
    switch (event.key) {
      case 'ArrowDown': next = step(1); break;
      case 'ArrowUp': next = step(-1); break;
      case 'Home': next = 0; break;
      case 'End': next = last; break;
      case 'Enter':
      case ' ': {
        // Let the row's own action button keep its activation.
        if (current < 0 || (event.target as HTMLElement).tagName === 'BUTTON') return;
        event.preventDefault();
        const rowId = options[current].getAttribute('data-row-id');
        if (rowId) onRowActivate?.(rowId);
        return;
      }
      default: return;
    }

    event.preventDefault();
    options[next].focus();
    const rowId = options[next].getAttribute('data-row-id');
    if (!rowId) return;
    onRowFocus?.(rowId);
    if (activateOnMove) onRowActivate?.(rowId);
  };

  return (
    <ul
      ref={listRef as React.RefObject<HTMLUListElement>}
      className={`workspace-list${className ? ` ${className}` : ''}`}
      role="listbox"
      aria-label={label}
      aria-multiselectable="false"
      tabIndex={tabIndex}
      onKeyDown={handleKeyDown}
    >
      {children}
    </ul>
  );
};

interface WorkspaceListRowProps {
  /** Identifies the row to WorkspaceList's keyboard navigation. */
  rowId?: string;
  /** Primary modality. Tints the lead glyph with the matching --cap-* token. */
  capability?: CapabilityIconTarget;
  title: React.ReactNode;
  /** Priority-5 metadata; hand it the status message whenever `status` is not
      nominal, so state is never carried by dot color alone. */
  meta?: React.ReactNode;
  /** Digits that line up down the column get mono + tabular figures. */
  metaMono?: boolean;
  /** Secondary modalities. Dropped first when the list gets narrow. */
  glyphs?: CapabilityIconTarget[];
  /** The fact that decides between two otherwise identical rows: the engine
      for a thing you might run, the time for a thing that happened. */
  anchor?: React.ReactNode;
  anchorTitle?: string;
  status?: WorkspaceListRowStatus;
  statusLabel?: string;
  /** 0–100. Draws the hairline along the row's bottom edge. */
  progress?: number;
  action?: WorkspaceListRowAction;
  selected?: boolean;
  /** Temporarily not selectable — a row mid-load, say. */
  disabled?: boolean;
  className?: string;
  id?: string;
  tabIndex?: number;
  ariaLabel?: string;
  ariaKeyShortcuts?: string;
  /** Domain attributes other code queries for, e.g. `data-model-id`. */
  dataAttributes?: Record<string, string>;
  onClick?: React.MouseEventHandler<HTMLLIElement>;
  onKeyDown?: React.KeyboardEventHandler<HTMLLIElement>;
  onFocus?: React.FocusEventHandler<HTMLLIElement>;
}

export const WorkspaceListRow: React.FC<WorkspaceListRowProps> = ({
  rowId,
  capability,
  title,
  meta,
  metaMono = false,
  glyphs,
  anchor,
  anchorTitle,
  status,
  statusLabel,
  progress,
  action,
  selected = false,
  disabled = false,
  className = '',
  id,
  tabIndex,
  ariaLabel,
  ariaKeyShortcuts,
  dataAttributes,
  onClick,
  onKeyDown,
  onFocus,
}) => {
  const leadColor = capability ? capabilityColor(capability) : undefined;
  const showMetaLine = Boolean(meta || anchor || (glyphs && glyphs.length > 0));
  const tone = rowTone(status);

  return (
    <li
      id={id}
      role="option"
      aria-selected={selected}
      aria-disabled={disabled || undefined}
      aria-label={ariaLabel}
      aria-keyshortcuts={ariaKeyShortcuts}
      tabIndex={tabIndex}
      className={`workspace-list-row${selected ? ' workspace-list-row--selected' : ''}${disabled ? ' workspace-list-row--disabled' : ''}${className ? ` ${className}` : ''}`}
      onClick={disabled ? undefined : onClick}
      onKeyDown={onKeyDown}
      onFocus={onFocus}
      data-row-id={rowId}
      {...dataAttributes}
    >
      {capability && (
        <span
          className="workspace-list-row__lead"
          style={leadColor ? ({ '--workspace-list-row-cap': leadColor } as React.CSSProperties) : undefined}
          aria-hidden="true"
        >
          <CapabilityIcon capability={capability} size={16} />
        </span>
      )}

      <span className="workspace-list-row__title">{title}</span>

      {showMetaLine && (
        <span className="workspace-list-row__meta">
          {meta && (
            <span
              className={`workspace-list-row__primary workspace-list-row__primary--${tone}${metaMono ? ' workspace-list-row__primary--mono' : ''}`}
            >
              {meta}
            </span>
          )}
          {glyphs && glyphs.length > 0 && (
            <span className="workspace-list-row__glyphs" aria-hidden="true">
              {glyphs.map(glyph => (
                <span key={glyph} title={CAPABILITY_TAG_LABELS[glyph as CapabilityTag]}>
                  <CapabilityIcon capability={glyph} size={12} />
                </span>
              ))}
            </span>
          )}
          {anchor && <span className="workspace-list-row__anchor" title={anchorTitle}>{anchor}</span>}
        </span>
      )}

      {status && (
        <span
          className={`workspace-list-row__status workspace-list-row__status--${status}`}
          title={statusLabel}
          aria-hidden="true"
        />
      )}

      {action && (
        <button
          type="button"
          className={`workspace-list-row__action${action.active ? ' workspace-list-row__action--active' : ''}`}
          onClick={event => { event.stopPropagation(); action.onClick(); }}
          aria-label={action.label}
          title={action.label}
          tabIndex={-1}
        >
          <Icon name={action.icon} size={12} aria-hidden="true" />
        </button>
      )}

      {progress != null && (
        <span className="workspace-list-row__progress" style={{ width: `${progress}%` }} aria-hidden="true" />
      )}
    </li>
  );
};

interface WorkspaceDetailPanelProps {
  ariaLabel: string;
  title?: React.ReactNode;
  leading?: React.ReactNode;
  metadata?: React.ReactNode;
  description?: React.ReactNode;
  actions?: React.ReactNode;
  headerExtras?: React.ReactNode;
  onBack?: () => void;
  backLabel?: string;
  backClassName?: string;
  onClose?: () => void;
  closeLabel?: string;
  closeClassName?: string;
  closeIcon?: IconName;
  className?: string;
  children: React.ReactNode;
}

export const WorkspaceDetailPanel = React.forwardRef<HTMLElement, WorkspaceDetailPanelProps>(({
  ariaLabel,
  title,
  leading,
  metadata,
  description,
  actions,
  headerExtras,
  onBack,
  backLabel = 'Back to list',
  backClassName = 'workspace-detail-panel__back',
  onClose,
  closeLabel = 'Close detail panel',
  closeClassName = 'workspace-detail-panel__close',
  closeIcon = 'x',
  className = '',
  children,
}, ref) => (
  <section ref={ref} className={`workspace-detail-panel${className ? ` ${className}` : ''}`} role="region" aria-label={ariaLabel}>
    {onBack && (
      <button type="button" className={backClassName} onClick={onBack} aria-label={backLabel}>
        <Icon name="chevron-right" size={14} className="workspace-detail-panel__back-icon" aria-hidden="true" /> {backLabel}
      </button>
    )}
    {(title !== undefined || actions || headerExtras) && <header className="workspace-detail-panel__header">
      {title !== undefined && (
        <div className="workspace-detail-panel__title-row">
          {leading && <div className="workspace-detail-panel__leading">{leading}</div>}
          <div className="workspace-detail-panel__identity">
            {title}
            {metadata && <WorkspaceMetadataGroup>{metadata}</WorkspaceMetadataGroup>}
          </div>
          {onClose && (
            <button type="button" className={closeClassName} onClick={onClose} aria-label={closeLabel}>
              <Icon name={closeIcon} size={16} aria-hidden="true" />
            </button>
          )}
        </div>
      )}
      {actions && <div className="workspace-detail-panel__action-bar">{actions}</div>}
      {headerExtras}
    </header>}
    {description && <div className="workspace-detail-panel__description">{description}</div>}
    {children}
  </section>
));

WorkspaceDetailPanel.displayName = 'WorkspaceDetailPanel';

interface WorkspaceDetailEmptyProps {
  icon: IconName;
  title: string;
  description: string;
}

export const WorkspaceDetailEmpty: React.FC<WorkspaceDetailEmptyProps> = ({ icon, title, description }) => (
  <div className="workspace-detail-panel workspace-detail-panel--empty" role="status">
    <div className="workspace-detail-panel__empty-copy">
      <span className="workspace-detail-panel__empty-icon" aria-hidden="true"><Icon name={icon} size={28} /></span>
      <h2>{title}</h2>
      <p>{description}</p>
    </div>
  </div>
);
