import React from 'react';
import { Icon, type IconName } from './Icon';

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

function flattenedMetadataChildren(children: React.ReactNode): React.ReactNode[] {
  return React.Children.toArray(children).flatMap(child => {
    if (React.isValidElement<{ children?: React.ReactNode }>(child) && child.type === React.Fragment) {
      return flattenedMetadataChildren(child.props.children);
    }
    return [child];
  });
}

export const WorkspaceMetadataGroup: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const orderedChildren = flattenedMetadataChildren(children)
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
    .map(({ child }) => child);

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

interface WorkspaceActionButtonProps extends React.ButtonHTMLAttributes<HTMLButtonElement> {
  appearance?: WorkspaceActionAppearance;
  size?: WorkspaceActionSize;
  icon?: IconName;
  iconOnly?: boolean;
}

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

interface WorkspaceActionLinkProps extends React.AnchorHTMLAttributes<HTMLAnchorElement> {
  appearance?: WorkspaceActionAppearance;
  size?: WorkspaceActionSize;
  icon?: IconName;
  iconOnly?: boolean;
}

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
    {title !== undefined && <header className="workspace-detail-panel__header">
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
      {description && <div className="workspace-detail-panel__description">{description}</div>}
      {headerExtras}
    </header>}
    {actions && <div className="workspace-detail-panel__action-bar">{actions}</div>}
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
