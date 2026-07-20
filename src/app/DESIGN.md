# Lemonade GUI design system

This document is the contract for Lemonade's desktop and web UI. It describes the current system, names the code that implements it, and sets the boundary between reusable product design and deliberately specialized feature UI.

## Principles

1. **The task determines the layout.** Product tabs use the same rail, list, and detail primitives. A tab may omit a panel when its task does not need one.
2. **Hierarchy comes from type, spacing, and surface level.** Extra rules, gradients, badges, and headings do not substitute for hierarchy.
3. **One meaning, one treatment.** Navigation, filtering, actions, metadata, status, and empty states keep the same visual grammar everywhere.
4. **Color communicates state.** Lemon yellow identifies the product and primary emphasis. Semantic colors are reserved for success, warning, danger, information, capabilities, backends, and charts.
5. **Specialization must earn its place.** A specialized treatment is acceptable when the underlying information is genuinely different, not merely because a tab was implemented separately.
6. **Desktop and mobile are the same information architecture.** Below 768 px, the contextual left rail becomes a dismissible panel opened by the title-bar menu button. It does not become a different navigation system.

## Standardized styles

### Source of truth and naming

- `src/styles/tokens.css` is the only source of visual constants.
- `src/styles/styles.css` implements components and feature layout with those tokens.
- Shared component classes use the `workspace-*` prefix. Feature classes may refine a shared component but must not redefine its typography, spacing scale, surface hierarchy, focus treatment, or control shape.
- React implementations of the shared panels and controls live in `src/components/WorkspacePanels.tsx`, `WorkspaceRailHeader.tsx`, and `WorkspaceMobileMenuButton.tsx`.
- Dynamic values such as progress percentage, measured resizer position, chart geometry, and backend identity may be passed through inline custom properties. Fixed visual values must use tokens or classes.

### Color

| Role | Tokens | Rule |
| --- | --- | --- |
| Layered surfaces | `--surface-base`, `--surface-1`, `--surface-2`, `--surface-3`, `--surface-raised`, `--surface-overlay` | Base is the canvas; higher numbers indicate stronger grouping or interaction. Adjacent panels use borders, not unrelated background colors. |
| Text | `--text-primary`, `--text-secondary`, `--text-tertiary`, `--text-disabled` | Primary for titles/values, secondary for body copy, tertiary for supporting copy, disabled only for unavailable content. |
| Product accent | `--accent`, `--accent-hover`, `--accent-fg`, `--accent-on`, `--accent-soft`, `--accent-strong`, `--accent-focus` | Primary actions, active navigation, and keyboard focus only. |
| Semantic state | `--success`, `--warn`, `--danger`, `--info` and `*-soft`; compatibility aliases `--status-ok`, `--status-warning`, `--status-error` | Never use a capability or backend color to communicate operational state. |
| Structure | `--border`, `--border-subtle`, `--border-strong` | Subtle separates panels and rows; strong indicates focus or emphasized selection. |
| Capability identity | `--cap-chat`, `--cap-vision`, `--cap-code`, `--cap-embedding`, `--cap-reranking`, `--cap-image`, `--cap-image-edit`, `--cap-audio`, `--cap-audio-generation`, `--cap-tts`, `--cap-model3d` | Used only for capability glyphs/chips. |
| Backend identity | `--backend-*` | Used only for compact backend marks. The backend helper returns these variables; components do not own hex values. |
| Monitoring data | `--chart-*` | Used only for charts, gauges, and their legends. Series color is stable within a chart. |

Both themes redefine the color roles; component selectors must not contain theme-specific product colors. Brand artwork and user/data-derived colors are the only exceptions.

### Typography

The UI uses `--font-sans`; code, logs, identifiers, and numeric payloads may use `--font-mono`.

| Role | Tokens | Code style |
| --- | --- | --- |
| Detail title | `--type-detail-title-size`, `--weight-semibold`, `--tracking-tight` | `.workspace-detail-panel__title` or a feature title inside `.workspace-detail-panel__identity` |
| Pane title | `--type-pane-title-size`, `--weight-semibold`, `--tracking-tight` | `.workspace-pane__header h1/h2`, `.workspace-list-panel__heading h1` |
| Panel title | `--type-panel-title-size`, `--weight-semibold` | `.monitor-subpanel__header h2` and card section titles |
| Body | `--type-body-size`, `--leading-normal` | Standard body and form value text |
| Supporting copy | `--type-supporting-size`, `--leading-snug` | Subtitles, descriptions, list metadata |
| Caption | `--type-caption-size` | Dense metadata and chips |
| Overline | `--type-overline-size`, `--tracking-caps`, uppercase, semibold | Rail context and control-group labels only; never as a redundant page heading |

A pane gets one visible title. Rail labels describe the rail's purpose (`History`, `Filters`, `Views`, `Settings`), not the tab name. Do not stack eyebrow, panel title, and page title when they repeat the same noun.

### Spacing and sizing

- All layout spacing uses the 4 px grid in `--space-*`; `--space-0-5` (2 px) and `--space-1-5` (6 px) are allowed for compact internal alignment.
- Panel padding uses `--panel-padding-inline` and `--panel-padding-block`. Dense list rows may use `--space-2`/`--space-3`.
- Controls use `--control-height-xs`, `--control-height-sm`, `--control-height-md`, or `--control-height-lg`. `--control-height` aliases the normal 36 px control.
- Icon-only toolbar controls use `--icon-button-size` and always have an accessible name and tooltip.
- `--workspace-header` gives list/pane headers the same 72 px rhythm. Headers do not gain arbitrary bottom rules: a divider appears only where scrollable content begins immediately below it.
- Content intended for reading or forms is bounded by `--content-form-width` or `--max-content-width`; operational tables and canvases may fill their pane.

### Radius, borders, and elevation

- `--radius-sm`: compact chips, badges, and dense cells.
- `--radius-md`: inputs, buttons, list selections, cards, and icon containers.
- `--radius-lg`: prominent empty-state or modal containers.
- `--radius-pill`: the primary tab selector, status dots, counters, and true pills only. Standard buttons are not pills.
- `--shadow-sm`: selected tab or lightweight floating control; `--shadow-md`: popover; `--shadow-lg`: modal or mobile sheet. Permanent panels use borders, not shadows.

### Icons

All UI icons render through `Icon.tsx`; Lucide geometry is the default and Simple Icons is limited to brands.

| Purpose | Icon |
| --- | --- |
| Mobile contextual menu | `menu` |
| Collapse/expand left panel | `panel-left-close` / `panel-left-open` |
| Create or compose | `compose` |
| Import from file | `file-up` |
| Download manager / download action | `download` |
| Filters | `funnel` |
| Settings | `settings` |
| Destructive action | `trash` |
| Close transient UI | `x` |
| Search | `search` |

Do not reuse the same icon for unrelated commands in one context. Text labels accompany actions whenever the pane is wide enough; icon-only controls are reserved for recognized toolbar actions.

### App chrome and navigation

- `.titlebar` is 52 px high. `.titlebar__nav` is centered independently of brand and utility widths and uses a rounded segmented-control treatment.
- On compact/mobile layouts the Lemonade brand is hidden, the contextual `menu` control occupies the left slot, the tab selector remains centered, and account/theme/download/server controls live under the `settings` menu.
- The top-level tabs are Chat, Models, Presets, Backends, Monitor, and Connect. Dashboard/Requests/Logs are Monitor views, not independent primary tabs.
- Active state is expressed with surface, text, and a restrained border; no active item should jump in size or position.

### Workspace layouts

The canonical information architecture is:

1. `.workspace-rail`: 248 px contextual history, filters, or navigation; collapses to 56 px on desktop and becomes `.mobile-context-panel` below 768 px.
2. `.workspace-list-panel`: optional single-column selection list, normally 304–360 px.
3. `.workspace-detail-panel` or `.workspace-pane`: task/detail area that consumes remaining width.

Use two panels when selection does not require a distinct list, and three when filtering/navigation, selection, and detail are separate tasks. `.workspace-pane__header`, `.workspace-list-panel__header`, and `.workspace-detail-panel__header` establish the same visual hierarchy at different levels.

### Shared controls and content

- Actions use `WorkspaceActionButton`, `WorkspaceActionLink`, and `WorkspaceActionGroup`. Appearances are `primary`, `secondary`, `quiet`, and `danger`; sizes are `small`, `medium`, and `toolbar`.
- Metadata uses `WorkspaceMetadataChip` inside `WorkspaceMetadataGroup`. Chips are ordered `high`, `medium`, then `low`; operational state precedes identity/capability, which precedes technical metadata and links.
- Standard forms use `.form-field`, `.form-field__label`, `.form-field__hint`, `.input`, `.select`, `.slider`, and the normal control tokens.
- Empty selection states use `WorkspaceDetailEmpty`. Empty states explain the next action; they do not decorate unused space.
- Focus is always visible through `--accent-focus`. Hover must not be the only way to discover an essential action.
- Motion uses `--duration-fast`, `--duration-normal`, `--duration-slow`, `--ease-out`, and `--ease-in-out`, and is disabled by the reduced-motion rule.

### Responsive behavior

- Desktop narrow: 769–1100 px; reduce panel padding and list width without changing control sizes or typography roles.
- Mobile/tablet: at 768 px and below, show one primary content panel at a time. Context rails are modal panels with backdrop, Escape handling, focus return, and a toggleable hamburger trigger.
- Phone: 480 px and below; keep touch targets at least 36 px, allow action labels to collapse only when accessible names remain, and avoid horizontally clipped forms or filter groups.

## Tab-specific styles

### Chat

**Layout:** two panels: `.workspace-rail` history and `.chat__main`. The composer is anchored within the main pane.

**Justified specialization:** conversational message rhythm, Markdown/code rendering, capability-specific composer controls, generated media, and the centered empty-state hero are unique to chat. The mobile history bottom sheet is justified because conversations are frequently switched while retaining draft context.

**Not justified:** independent rail header, button, badge, or mobile-menu geometry. These must inherit the workspace tokens and shared controls. Capability color is semantic, never decorative.

### Models

**Layout:** three panels: `.model-nav-rail` filter rail, `.model-list-panel.workspace-list-panel`, and `.model-detail-panel.workspace-detail-panel`. The list may be resized on desktop.

**Justified specialization:** dense model availability rows, provider search results, download progress, backend identity marks, README/files/tuning tabs, and the resizer reflect model-management data.

**Not justified:** custom list/detail backgrounds, headings, action buttons, metadata order, editor shells, or filter sizing. Model and preset list/detail panels share the workspace grammar. Custom model, router, and global-settings editors occupy the same detail shell.

### Presets

**Layout:** three panels: `.context-rail--autoopt` filter/optimization rail, `.preset-list-panel.workspace-list-panel`, and `.preset-detail-panel.workspace-detail-panel`.

**Justified specialization:** intent chips, linked-model state, AutoOpt progress, and parameter editors communicate preset-specific concepts.

**Not justified:** card-grid presentation in the list, modal-like detail styling on desktop, or unique creation-form hierarchy. The library is a single-column list matching Models; creation and editing use the shared detail shell. `compose` creates, while `file-up` imports.

### Backends

**Layout:** two panels: filter `.workspace-rail` and `.workspace-pane` compatibility matrix.

**Justified specialization:** the matrix is the clearest representation of device × capability support. Backend identity colors are allowed only as small stable identifiers.

**Not justified:** bespoke filter rows, page header, status controls, banners, or button geometry. They use workspace filters, pane headers, semantic state, and shared controls.

### Monitor

**Layout:** the first rail selects Overview, Requests, or Logs. Overview uses one content pane; Requests and Logs use a second functional filter/list subpanel plus a detail/output pane.

**Overview — justified specialization:** charts, gauges, metric cards, tabular numerals, and stable `--chart-*` series colors. Glow and ornamental gradients are not justified.

**Requests — justified specialization:** trace waterfall, metric strip, prompt diff, and replay/improvement workspaces. These are dense diagnostic artifacts. Their surrounding header, tabs, forms, buttons, cards, and modals still use system tokens.

**Logs — justified specialization:** monospace virtualized output, severity markers, and compact fixed-height rows. The filter panel, search control, header, and actions are standard workspace UI.

### Connect

**Layout:** two panels: Settings `.workspace-rail` and a `.workspace-pane` for the selected Server, Storage, Cloud, MCP, Apps, Support, or Account section.

**Justified specialization:** provider brand marks, endpoint examples, and marketplace metadata. Forms use a bounded reading width.

**Not justified:** card-heavy pages, independent headings inside an already titled pane, custom input heights, or unique help-link styling. Sections use the pane header, standard forms/actions, and restrained row/card grouping.

## Change checklist

Before merging a GUI change:

- The tab still follows the rail/list/detail decision above.
- A new fixed value was added to `tokens.css` only if no existing token expresses the role.
- A new component style represents a new semantic concept, not a renamed copy of an existing one.
- Light and dark themes both preserve hierarchy and contrast.
- The compact and mobile layouts expose every rail function through the same menu pattern.
- Keyboard focus, accessible names, reduced motion, empty/loading/error states, and label truncation were verified.
- Models and Presets were compared side by side; Monitor and Connect rails were compared side by side.
