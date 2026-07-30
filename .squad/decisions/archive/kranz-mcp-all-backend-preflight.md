# MCP all-backend preflight decision

Recorded: 2026-07-30T10:51:42.882-06:00
Owner: Kranz

## Decision

The MCP load guard now uses Router::resolve_effective_recipe_options() with the exact ModelInfo and request RecipeOptions that will be passed to Router::load_model(). It reads the resulting <recipe>_backend value and checks only that selected backend through SystemInfo::get_all_recipe_statuses(), accepting only the installed state. Missing, unsupported, installable, or otherwise unavailable selections return the existing structured MCP tool error before Router::load_model(). Cloud remains exempt because its load path has no local executable backend.

## Sources inspected

The effective selection includes request options, ModelInfo::recipe_options, architecture defaults, and server RuntimeConfig recipe options. The checked backend source is the resolved <recipe>_backend field; status comes from the read-only SystemInfo recipe/backend state table. The guard does not call backend installation, model download, executable download, or release-management APIs.

## Validation

The actual model registry supplies ModelInfo recipes, and defaults.json supplies the server backend defaults consumed by Router's resolver. The Windows lemonade-server-core target built successfully after the MCP-only change.
