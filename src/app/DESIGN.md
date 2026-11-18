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

- `src/styles/tokens.css` is the source of shared and repeated visual constants.
- `src/styles/styles.css` implements components and feature layout with those tokens.
- Shared component classes use the `workspace-*` prefix. Feature classes may refine a shared component but must not redefine its typography, spacing scale, surface hierarchy, focus treatment, or control shape.
- React implementations of the shared panels and controls live in `src/components/WorkspacePanels.tsx`, `WorkspaceRailHeader.tsx`, and `WorkspaceMobileMenuButton.tsx`.
- Every list renders `WorkspaceList` + `WorkspaceListRow` (`.workspace-list` / `.workspace-list-row`). A tab supplies the row's content, never its geometry, typography, status idiom, or selection treatment.
- Dynamic values such as progress percentage, measured resizer position, chart geometry, and backend identity may be passed through inline custom properties. Repeated fixed values use tokens; feature-local geometry stays in its named class.

### Color

| Role | Tokens | Rule |
| --- | --- | --- |
| Layered surfaces | `--surface-base`, `--surface-1`, `--surface-2`, `--surface-3`, `--surface-raised`, `--surface-overlay` | Base is the canvas; higher numbers indicate stronger grouping or interaction. Adjacent panels use borders, not unrelated background colors. |
| Text | `--text-primary`, `--text-secondary`, `--text-tertiary`, `--text-disabled` | Primary for titles/values, secondary for body copy, tertiary for supporting copy, disabled only for unavailable content. |
| Product accent | `--accent`, `--accent-hover`, `--accent-fg`, `--accent-on`, `--accent-soft`, `--accent-strong`, `--accent-surface`, `--accent-border`, `--accent-focus` | Primary actions, active navigation, and keyboard focus only. |
| Semantic state | `--success`, `--warn`, `--danger`, `--info` and `*-soft`; `--danger-on`; compatibility aliases `--status-ok`, `--status-warning`, `--status-error` | Never use a capability or backend color to communicate operational state. |
| Structure | `--border`, `--border-subtle`, `--border-strong` | Subtle separates panels and rows; strong indicates focus or emphasized selection. |
| Capability identity | `--cap-chat`, `--cap-vision`, `--cap-code`, `--cap-embedding`, `--cap-reranking`, `--cap-image`, `--cap-image-edit`, `--cap-audio`, `--cap-audio-generation`, `--cap-tts`, `--cap-model3d` | Used only for capability glyphs/chips. |
| Provider identity | `--provider-hugging-face`, `--provider-hugging-face-fg`, `--provider-modelscope`, `--provider-modelscope-fg` | Stable provider marks and compact provider results only; never status or general decoration. |
| Backend identity | `--backend-*` | Used only for compact backend marks. The backend helper returns these variables; components do not own hex values. |
| Monitoring data | `--chart-*` | Used only for charts, gauges, and their legends. Series color is stable within a chart. |

Both themes redefine the color roles; component selectors must not contain theme-specific product colors. Brand artwork and user/data-derived colors are the only exceptions.

Identity and chart hues are theme-specific too. A value tuned against the dark canvas typically lands near 2:1 on the light one, so every `--cap-*`, `--backend-*`, `--chart-*`, and provider foreground token carries a darkened light-theme value that clears the 3:1 floor WCAG 1.4.11 sets for non-text UI components. A new identity color is not finished until both themes are declared and measured.

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

- Shared layout spacing uses the 4 px grid in `--space-*`; `--space-0-5` (2 px) and `--space-1-5` (6 px) support compact internal alignment. One-pixel structure and feature-local optical alignment may remain in the owning class.
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
- `--shadow-sm`: selected tab or lightweight floating control; `--shadow-md`: popover; `--shadow-lg`: modal or mobile sheet; `--shadow-top`: an anchored bottom sheet or footer. Permanent panels use borders, not shadows.

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
- The top-level tabs are Chat, Models, Backends, Monitor, and Settings. Performance/Telemetry/Logs are Monitor views, not independent primary tabs.
- Active state is expressed with surface, text, and a restrained border; no active item should jump in size or position.

### Workspace layouts

The canonical information architecture is:

1. `.workspace-rail`: 248 px contextual history, filters, or navigation; collapses to 56 px on desktop and becomes `.mobile-context-panel` below 768 px.
2. `.workspace-list-panel`: optional single-column selection list, normally 304–360 px.
3. `.workspace-detail-panel` or `.workspace-pane`: task/detail area that consumes remaining width.

Use two panels when selection does not require a distinct list, and three when filtering/navigation, selection, and detail are separate tasks. `.workspace-pane__header`, `.workspace-list-panel__header`, and `.workspace-detail-panel__header` establish the same visual hierarchy at different levels.

### Selection lists

Every list the user selects from — the model catalog, remote registry results, chat history, captured requests, and the composer's model picker — is one 52 px row on a two-column grid: a 16 px lead glyph and a flexible two-line body. Only the body flexes, so nothing shifts between states. A list inside a popover is still this list: the picker adds only the popover frame and a search field that hands ArrowDown to the list.

| Line | Slot | Holds | Priority |
| --- | --- | --- | --- |
| — | Lead | Primary modality, tinted with its `--cap-*` token | Never dropped |
| Title | Title | The row's identity, `--text-sm` / `--weight-medium` | Never dropped |
| Meta | Status **or** facts | An in-flight state and its words, **or** the secondary glyphs and measurements — never both | Status never dropped; glyphs dropped first |
| Meta | Anchor | The fact that decides between two otherwise identical rows: the engine for a thing you might run, the time for a thing that happened | Never dropped |
| — | Action | One row-scoped command, overlaying the right edge at full row height | — |

The meta line holds one thing at a time. A row that is downloading, running, generating, or asking for attention gives the whole line to its status; every other row gives it to glyphs and facts. Both competing for one line is what made a dot and a lead glyph fight for the same glance.

Hover and selection are **translucent, never an opaque surface value**. The lists do not share a ground — the catalog scrolls over `--surface-base`, the chat and telemetry rails are `--surface-1`, the composer's picker floats on `--surface-raised`, and the light theme collapses the first two onto one color — so a color tuned against one list is invisible or inverted on another. A scrim composites the same lift over all of them and self-corrects per theme.

Four rules keep the grammar honest. **The dot and its words are one unit** — a mark placed away from the thing it marks leaves color carrying state on its own, so `.workspace-list-row__status` renders both together at the head of the meta line and takes the state's tone. **The title owns its line.** It is what the eye scans, so nothing shares its measure — the anchor ends the meta line instead, alongside the metadata it ranks with. **The action overlays the row's right edge at full height** rather than holding a column, and stays hidden until the row is hovered or focused; the anchor then goes invisible without giving up its width, so the meta line cannot reflow. A column reserved for a control on every row put a button beside every timestamp and engine label in the app and stopped the anchor short of the edge. The title reserves the action's width at its end, because space reserved on a left-aligned, ellipsised run costs nothing visible, where the same reservation on a right-aligned label reads as misalignment. An action may be **latched**, holding the edge permanently and replacing the anchor: a pinned model shows its pin instead of its engine, a loaded model in the picker always offers eject, and a download in flight always offers cancel — in each case the control is the more useful fact about that row and must not need a hover to find. Coarse pointers cannot hover at all, so there the action takes the edge outright — reusing the latched visual rather than returning the button to flow, which would auto-place it into an implicit third grid row. **Rows degrade by priority, not position** — `@container` queries on `.workspace-list` drop the secondary glyphs, never the name, modality, anchor, or status.

**Status is transient, and a steady state is a section.** Only `live`, `busy`, `attention`, and `error` render — `Downloading 62%`, `Running`, `Generating`, `Engine update required`, `Failed`. A row that is simply downloaded, or simply available, shows nothing: repeating one of two constants down a whole column is noise, and it competes with the lead glyph for the same glance. What every row in a run shares belongs on a `WorkspaceListGroup` heading instead — the catalog reads `Pinned`, `Downloaded`, `Not downloaded`, matching the `Hugging Face` and `ModelScope` headings the registry results already carry. A section always states the truth about its rows, so grouping is structural and the chosen sort orders rows *within* each section rather than deciding the sections.

When status does render it owns the line, so a caller composes whatever words belong there — a slow captured request keeps its metrics (`Slow · 4.8s · 210 tok`) because that is precisely when they are evidence. A successful request stays wordless: `OK` restates the dot without adding anything.

A list is either **chosen from** or **read**. The catalog, registry, history, picker, and captured requests are listboxes of options. Monitor's loaded-models readout is a plain list whose rows carry their own eject control — `selectable={false}` on both the list and its rows renders `list`/`listitem` instead of `listbox`/`option`. The row template is identical; only the semantics change. This is not a style choice: a button nested inside `role="option"` is an illegal nested interactive control, and a readout that announces itself as a selection widget misdescribes what it is.

`WorkspaceList` owns the ARIA listbox keyboard contract — roving tabindex, Arrow/Home/End, Enter/Space — for every list. A list supplies policy (`wrap`, `activateOnMove`) and two callbacks (`onRowFocus`, `onRowActivate`); rows are matched by `data-row-id`. A tab must not re-implement listbox navigation, and the row's tone follows from its `status` rather than being passed alongside it. A tab supplies `statusText` for the words and `meta` for the facts; it must not pack state into the facts slot.

### Shared controls and content

- Actions use `WorkspaceActionButton`, `WorkspaceActionLink`, and `WorkspaceActionGroup`. Appearances are `primary`, `secondary`, `quiet`, and `danger`; sizes are `small`, `medium`, and `toolbar`.
- Metadata uses `WorkspaceMetadataChip` inside `WorkspaceMetadataGroup`. Chips are ordered `high`, `medium`, then `low`; operational state precedes identity/capability, which precedes technical metadata and links.
- Standard forms use `.form-field`, `.form-field__label`, `.form-field__hint`, `.input`, `.select`, `.slider`, and the normal control tokens. Native selects use the theme-aware `--select-chevron` asset token.
- Empty selection states use `WorkspaceDetailEmpty`. Empty states explain the next action; they do not decorate unused space.
- Focus is always visible through `--accent-focus`. Hover must not be the only way to discover an essential action.
- Motion uses `--duration-fast`, `--duration-normal`, `--duration-slow`, `--ease-out`, and `--ease-in-out`, and is disabled by the reduced-motion rule.

### Responsive behavior

- Desktop narrow: 769–1100 px; reduce panel padding and list width without changing control sizes or typography roles.
- Mobile/tablet: at 768 px and below, show one primary content panel at a time. Context rails are modal panels with backdrop, Escape handling, focus return, and a toggleable hamburger trigger.
- Phone: 480 px and below; keep touch targets at least 36 px, allow action labels to collapse only when accessible names remain, and avoid horizontally clipped forms or filter groups.

The 768 px and 480 px boundaries are the only ones JavaScript may test. They live in `src/styles/breakpoints.ts` as `MOBILE_BREAKPOINT` and `PHONE_BREAKPOINT`; `styles.css` repeats them as literals because custom properties cannot appear inside `@media`. A layout check that disagrees with its media query leaves a band of widths where the two layouts fight, so JS must read the shared constant rather than inline a number. Components may still define their own intermediate reflow points (for example 900 px or 720 px for a grid that runs out of room early); those are presentation-only and never gate behavior.

## Tab-specific styles

The labels below are normative boundaries, not backlog states. **Justified specialization** names feature UI that may remain distinct. **Prohibited variation** names divergence that the implementation must not contain and that future changes must not reintroduce.

### Chat

**Layout:** two panels: `.workspace-rail` history and `.chat__main`. The composer is anchored within the main pane.

**Justified specialization:** conversational message rhythm, Markdown/code rendering, capability-specific composer controls, generated media, and the centered empty-state hero are unique to chat. The mobile history bottom sheet is justified because conversations are frequently switched while retaining draft context.

**Prohibited variation:** independent rail header, button, badge, or mobile-menu geometry. These must inherit the workspace tokens and shared controls. Capability color is semantic, never decorative. Conversation rows and the composer's model picker are both the shared selection list; capability and generating state belong in the lead glyph and the status dot, not in per-row pills, and eject belongs in the row action slot.

### Models

**Layout:** three panels: `.model-nav-rail` filter rail, `.model-list-panel.workspace-list-panel`, and `.model-detail-panel.workspace-detail-panel`. The list may be resized on desktop.

**Justified specialization:** download progress, backend identity marks, README/files/tuning tabs, and the resizer reflect model-management data.

**Prohibited variation:** custom list/detail backgrounds, headings, action buttons, metadata order, editor shells, or filter sizing. Model list/detail panels share the workspace grammar. Custom model, router, and global-settings editors occupy the same detail shell. Built-in catalog rows and remote registry results are the shared selection list — a taller registry row, a provider tile repeating what the zone header already says, or tag chips inside a row are not model-management data, they are a second row design.

### Backends

**Layout:** two panels: filter `.workspace-rail` and `.workspace-pane` compatibility matrix.

**Justified specialization:** the matrix is the clearest representation of device × capability support. Backend identity colors are allowed only as small stable identifiers.

**Prohibited variation:** bespoke filter rows, page header, status controls, banners, or button geometry. They use workspace filters, pane headers, semantic state, and shared controls.

### Monitor

**Layout:** the first rail selects Performance, Telemetry, or Logs. Performance uses one content pane; Telemetry and Logs use a second functional filter/list subpanel plus a detail/output pane.

**Performance — justified specialization:** charts, gauges, metric cards, tabular numerals, and stable `--chart-*` series colors. Glow and ornamental gradients are prohibited.

**Performance — the loaded-models card** renders the shared list with `selectable={false}`: it is a readout, not a selection. Every row in it is loaded, so no row carries a status; each offers a latched eject.

**Telemetry — justified specialization:** trace waterfall, metric strip, prompt diff, and replay/improvement workspaces. These are dense diagnostic artifacts. Their surrounding header, tabs, forms, buttons, cards, and modals still use system tokens. The request list is the shared selection list; it keeps `--font-mono` and tabular figures on its metrics because those digits are compared down a column.

**Telemetry — prohibited variation:** a per-row kind pill, a second status dot, or a redundant `OK` label. Request kind belongs in the lead glyph and status in the one shared dot, with any non-nominal state named in words.

**Logs — justified specialization:** monospace virtualized output, severity markers, and compact fixed-height rows. The filter panel, search control, header, and actions are standard workspace UI.

### Apps

**Layout:** two panels: an App types `.workspace-rail` and a full-width `.workspace-pane` directory. Marketplace categories live in the rail; search remains a pane-level action. The directory is not centered or embedded in Settings.

**Justified specialization:** marketplace logos, category metadata, and external Visit, Guide, and Video actions.

**Prohibited variation:** horizontal category chip bars, centered fixed-width page shells, duplicate Settings navigation entries, or app-specific rail/mobile behavior. Apps uses the shared workspace rail, pane header, resource rows, actions, and mobile context panel.

### Connect

**Layout:** two panels: Settings `.workspace-rail` and a `.workspace-pane` for the selected Server, Storage, Cloud, MCP, Support, or Account section.

**Justified specialization:** provider brand marks and endpoint examples. Forms use a bounded reading width.

**Prohibited variation:** card-heavy pages, independent headings inside an already titled pane, custom input heights, or unique help-link styling. Sections use the pane header, standard forms/actions, and restrained row/card grouping.

## Conformance audit

The implementation was audited against every boundary above on 2026-07-20. The current system conforms as follows:

| Area | Implemented boundary |
| --- | --- |
| App chrome | One centered rounded primary selector; one contextual mobile menu position; compact utilities live under the settings control; the decorative lemon icon is not rendered. |
| Rails and mobile context | Chat, Models, Backends, Apps, Monitor, and Settings use `WorkspaceRailHeader` and `WorkspaceMobileMenuButton`. Mobile panels share the same dialog, backdrop, focus-return, Escape, and toggle behavior. |
| Backends | The filter rail, pane header, status actions, and buttons use workspace components. Only the device × capability matrix and compact backend identity marks remain specialized. |
| Apps | Category navigation lives in a dedicated rail; the directory uses the full content pane with shared search, resource rows, actions, and responsive context-panel behavior. |
| Monitor | Performance retains charts and gauges without ornamental glow. Telemetry and Logs retain diagnostic artifacts while their navigation, filters, forms, and actions use the workspace grammar. |
| Connect | Settings navigation, pane headers, fields, actions, provider/help rows, and bounded content use shared components and control sizing. |
| CSS integrity | Component colors resolve through tokens, `styles.css` contains no literal product colors, and the audit contains no exact duplicate rule blocks in the same cascade context. Remaining `!important` declarations are limited to third-party SVG theming, reduced-motion enforcement, mobile browser input behavior, and measured/virtualized layout overrides. |

“Prohibited variation” entries remain in this specification after conformance because they are regression guards, not unresolved work.

## Change checklist

Before merging a GUI change:

- The tab still follows the rail/list/detail decision above.
- A new fixed value was added to `tokens.css` only if no existing token expresses the role.
- A new component style represents a new semantic concept, not a renamed copy of an existing one.
- Light and dark themes both preserve hierarchy and contrast.
- The compact and mobile layouts expose every rail function through the same menu pattern.
- Keyboard focus, accessible names, reduced motion, empty/loading/error states, and label truncation were verified.
