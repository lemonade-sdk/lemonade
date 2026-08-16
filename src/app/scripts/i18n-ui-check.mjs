import fs from 'node:fs';
import path from 'node:path';
import { projectRoot } from './i18n-lib.mjs';

function hasTranslation(resource, key) {
  let value = resource;
  for (const part of key.split('.')) {
    if (!value || typeof value !== 'object' || !Object.prototype.hasOwnProperty.call(value, part)) return false;
    value = value[part];
  }
  return value !== undefined;
}

function requireTranslations(source, namespace, keys) {
  const resource = source.resources[namespace];
  if (!resource) throw new Error(`Source locale is missing ${namespace}.json.`);
  const missing = keys.filter(key => !hasTranslation(resource, key));
  if (missing.length > 0) {
    throw new Error(`Source locale is missing dynamic ${namespace} keys: ${missing.join(', ')}`);
  }
}

function readSource(relativePath) {
  return fs.readFileSync(path.join(projectRoot, relativePath), 'utf8');
}

export function checkUiI18nContracts(source) {
  const i18nLib = readSource('scripts/i18n-lib.mjs');
  if (i18nLib.includes('import.meta.dirname') || !i18nLib.includes('fileURLToPath(import.meta.url)')) {
    throw new Error('i18n-lib.mjs must resolve its directory with the Node-20-compatible fileURLToPath(import.meta.url) pattern.');
  }

  requireTranslations(source, 'apps', [
    'unknownError',
  ]);

  requireTranslations(source, 'models', [
    ...['chat', 'omni', 'image', 'audio', 'audio-generation', 'tts', 'model3d', 'embedding', 'reranking', 'classification']
      .map(capability => `manager.custom.capabilities.${capability}`),
    'nav.tasks.classification',
    'detail.main.capabilityClassification',
    'nav.builtInTags.recommended',
    'nav.builtInTags.hot',
    'nav.builtInTags.small',
    'list.readiness.withBackend',
    ...['downloading', 'paused', 'completed', 'error', 'cancelled', 'deleting']
      .map(status => `downloads.statuses.${status}`),
  ]);

  requireTranslations(source, 'common', [
    'capabilityTags.additional',
    ...['hot', 'popular', 'chat', 'omni', 'vision', 'tool', 'reasoning', 'code', 'audio', 'audio-generation', 'image', 'tts', 'model3d', 'embedding', 'reranking', 'classification']
      .map(tag => `capabilityTags.${tag}`),
  ]);

  requireTranslations(source, 'backends', [
    'messages.supportedNotInstalled',
    'messages.updateRequiredBeforeUse',
    'messages.automaticInstallDisabled',
    'system.versionUnknown',
    'system.osUnknown',
    'toast.unknownInstallError',
  ]);

  requireTranslations(source, 'inspect', [
    'diagnostics.executionError.clientDisconnected',
  ]);

  requireTranslations(source, 'settings', [
    'server.urlRequired',
    'server.urlProtocol',
    'server.invalidUrl',
  ]);


  requireTranslations(source, 'monitor', [
    'dashboard.versionUnknown',
  ]);

  requireTranslations(source, 'chat', [
    ...['list_models', 'get_model_info', 'load_model', 'unload_model', 'get_loaded_models', 'get_server_health', 'pull_model', 'delete_model', 'get_system_info', 'list_backends', 'install_backend', 'ask_question', 'generate_image', 'edit_image', 'generate_audio', 'text_to_speech', 'transcribe_audio', 'generate_3d', 'analyze_image']
      .map(tool => `toolLabels.${tool}`),
    'model.modes.classification',
    'stream.callingTools',
    'stream.stopped',
    'errors.toolLoopLimit',
    'errors.toolExecutionFailed',
    'toolResults.error',
    'toolResults.inventoryRetrieved',
    'toolResults.backendInstalled',
    'toolResults.imageGenerated',
    'toolResults.imageEdited',
    'toolResults.audioGenerated',
    'toolResults.speechGenerated',
    'toolResults.audioTranscribed',
    'toolResults.model3dGenerated',
    'toolResults.imageAnalyzed',
    'toolResults.modelLoadedNamed',
    'toolResults.modelUnloadedNamed',
    'toolResults.modelDeletedNamed',
    'toolResults.noModelsLoaded',
    'toolResults.backendDefault',
    'toolResults.noBackends',
    'toolResults.systemBackendSummary',
    ...['status', 'capability', 'size', 'context', 'checkpoint', 'recipes', 'components', 'options', 'version', 'os', 'source', 'variant', 'recipe', 'device', 'type', 'file', 'downloads']
      .map(field => `toolResults.fields.${field}`),
    ...['loaded', 'downloaded', 'registry'].map(status => `toolResults.statuses.${status}`),
    ...['chat', 'omni', 'image', 'audio', 'audio-generation', 'tts', 'model3d', 'embedding', 'reranking', 'classification']
      .map(capability => `toolResults.capabilities.${capability}`),
    ...['installed', 'installable', 'unsupported', 'update_required', 'update_available', 'action_required', 'unknown']
      .map(state => `toolResults.backendStates.${state}`),
  ]);


  const appsView = readSource('src/components/AppsView.tsx');
  if (!appsView.includes("marketplaceError === 'Unknown error' ? t('unknownError')")) {
    throw new Error('AppsView must localize the generic marketplace Unknown error fallback.');
  }

  const backendManager = readSource('src/components/BackendManager.tsx');
  for (const [message, key] of [
    ['backend is supported but not installed', 'messages.supportedNotInstalled'],
    ['backend update is required before use', 'messages.updateRequiredBeforeUse'],
    ['automatic backend install is disabled', 'messages.automaticInstallDisabled'],
  ]) {
    if (!backendManager.includes(`'${message}': '${key}'`)) {
      throw new Error(`BackendManager is missing the canonical server-message mapping for: ${message}`);
    }
  }
  for (const placeholder of ['unknown', 'null', 'undefined', 'none', 'n/a']) {
    if (!backendManager.includes(`'${placeholder}'`)) {
      throw new Error(`BackendManager cleanString no longer filters placeholder value: ${placeholder}`);
    }
  }
  if (!backendManager.includes("normalized === 'unknown backend install error'")) {
    throw new Error('BackendManager must localize the API fallback for unknown backend install errors.');
  }
  if (!backendManager.includes('type BackendToast =')
      || backendManager.includes('toast(t(')
      || backendManager.includes('error: message,')
      || !backendManager.includes('backendToastText(toastMsg, t)')) {
    throw new Error('BackendManager toasts and install errors must remain semantic/raw until presentation.');
  }

  const modelManager = readSource('src/components/ModelManager.tsx');
  if (modelManager.includes('labelDisplay(')) {
    throw new Error('ModelManager still references the removed hard-coded labelDisplay helper.');
  }
  for (const stale of [
    'Editing saved definition',
    'Register a model or collection that is not included in the Lemonade catalog.',
    '>Close</WorkspaceActionButton>',
    '>Export</WorkspaceActionButton>',
    '>Import</WorkspaceActionButton>',
    '                    Custom Model',
    '                    Omni Collection',
  ]) {
    if (modelManager.includes(stale)) throw new Error(`ModelManager contains stale English custom-editor UI: ${stale}`);
  }

  const modelNav = readSource('src/components/ModelNavRail.tsx');
  for (const mapping of [
    "Recommended: 'nav.builtInTags.recommended'",
    "Hot: 'nav.builtInTags.hot'",
    "Small: 'nav.builtInTags.small'",
  ]) {
    if (!modelNav.includes(mapping)) {
      throw new Error(`ModelNavRail is missing localized built-in tag mapping: ${mapping}`);
    }
  }

  const modelPresentation = readSource('src/modelPresentation.ts');
  if (modelPresentation.includes("recipe || 'Unknown'")) {
    throw new Error('modelPresentation still exposes an English Unknown fallback for empty recipe labels.');
  }

  const dashboard = readSource('src/components/Dashboard.tsx');
  if (dashboard.includes('`Lemonade ${health.version}`')) {
    throw new Error('Dashboard still renders the raw server version without a localized unknown fallback.');
  }

  const modelDetail = readSource('src/components/ModelDetailPanel.tsx');
  if (modelDetail.includes('custom.form.capabilities.')) {
    throw new Error('ModelDetailPanel uses stale custom.form.capabilities.* keys.');
  }
  if (modelDetail.includes("cap === 'chat' || cap === 'unknown'")) {
    throw new Error('Unknown model capabilities must not inherit chat/llama.cpp tuning fields implicitly.');
  }

  // Locale is presentation state. It must not become a dependency of effects
  // that hydrate drafts, reset editor state, or perform network polling.
  const connectView = readSource('src/components/ConnectView.tsx');
  if (connectView.includes('}, [t]);\n\n  useEffect(() => {\n    if (!isActive) return;')) {
    throw new Error('Locale changes must not rehydrate ConnectView drafts or refresh providers.');
  }
  if (!connectView.includes('type ConnectMessage =')
      || connectView.includes('setNotice(t(')
      || connectView.includes('setCloudNotice(t(')
      || connectView.includes('setDirectoryNotice(t(')
      || connectView.includes('setError(friendlyErrorMessage(')
      || connectView.includes('setCloudError(friendlyErrorMessage(')
      || connectView.includes('setDirectoryError(friendlyErrorMessage(')) {
    throw new Error('ConnectView must store semantic presentation state and translate notices only at render time.');
  }

  requireTranslations(source, 'router', [
    'picker.selectModel', 'picker.searchModels', 'picker.noMatches', 'picker.staleValue',
    'graph.toolbox', 'graph.logicGates', 'graph.conditions', 'graph.emptyTitle',
    'graph.inspectorTitle', 'graph.errors.duplicate',
    'editor.confirm.switch.title', 'editor.confirm.delete.title',
    'managerDelete.title', 'managerDelete.offlineWarning',
    'validation.routerNameRequired', 'validation.ruleRequired',
  ]);

  const routerEditor = readSource('src/components/RouterEditorPanel.tsx');
  const routerPicker = readSource('src/components/RouterModelPicker.tsx');
  const routerNodeEditor = readSource('src/components/RouterNodeEditor.tsx');
  const routerGraph = readSource('src/components/RouterRuleGraph.tsx');
  if (!routerPicker.includes("useI18n('router')") || !routerNodeEditor.includes("useI18n('router')") || !routerGraph.includes("useI18n('router')")) {
    throw new Error('All Router editor sub-surfaces must resolve presentation through the router namespace.');
  }
  if (!routerGraph.includes('localizeRouterGraphError') || !routerEditor.includes('localizeRouterValidationMessage') || !routerEditor.includes('localizeRouterImportError')) {
    throw new Error('Router domain diagnostics must be localized at the presentation boundary.');
  }
  for (const stale of ['<h3>Candidate Models</h3>', '<h3>Routing Strategy</h3>', '<strong>Natural-Language Router</strong>']) {
    if (routerEditor.includes(stale)) throw new Error(`RouterEditor contains stale English UI: ${stale}`);
  }

  if (routerEditor.includes('}, [initialModel, t]);') || !routerEditor.includes('routerTranslateRef.current = t;')) {
    throw new Error('Locale changes must not reset RouterEditor drafts or recreate its provider refresh callback.');
  }
  if (!routerEditor.includes('type RouterNotice =')
      || /setNotice\(\s*t\(/.test(routerEditor)
      || routerEditor.includes("savingProvider ? 'Saving…' : 'Save'")
      || !routerEditor.includes('routerNoticeText(notice, t)')) {
    throw new Error('RouterEditor notices must remain semantic and translate at presentation time.');
  }

  const improveTab = readSource('src/components/inspect/ImproveTab.tsx');
  if (improveTab.includes('}, [selectedTrace.id, t]);')) {
    throw new Error('Locale changes must not reset Inspect Improve test state.');
  }

  const dashboardData = readSource('src/hooks/useDashboardData.ts');
  if (dashboardData.includes('}, [computeAggregates, t]);')) {
    throw new Error('Dashboard polling must not depend on the translation function.');
  }
  if (!dashboardData.includes('if (!isActive || paused)')) {
    throw new Error('Paused Dashboard polling must return before calling poll().');
  }

  const mcpPanel = readSource('src/components/McpPanel.tsx');
  if (!mcpPanel.includes('mcpTranslateRef.current = t;') || mcpPanel.includes("return 'unavailable';\n  }, [t]);")) {
    throw new Error('Locale changes must not retrigger MCP access probing.');
  }

  if (!modelDetail.includes('routerCollectionTranslateRef.current = t;')) {
    throw new Error('Locale changes must not retrigger router collection provider loading.');
  }
  if (modelDetail.includes('Image unavailable:') || modelDetail.includes("'README image unavailable'")) {
    throw new Error('README image fallback text must not be cached in English.');
  }
  if (!modelDetail.includes('}, [html, t]);')) {
    throw new Error('README image failure presentation must track the current locale.');
  }

  const marketplacePresentation = readSource('src/features/apps/marketplacePresentation.ts');
  if (marketplacePresentation.includes('.toLocaleLowerCase()')) {
    throw new Error('Marketplace translation keys must use deterministic host-independent case folding.');
  }

  if (!dashboard.includes("formatRelativeTime(0, 'second', { numeric: 'auto' })")) {
    throw new Error('Dashboard just-now formatting must pass zero to Intl.RelativeTimeFormat.');
  }
  for (const staleNumberFormatting of ['latestTps.toFixed(1)', 'ramUsedGb.toFixed(1)', 'stats.tokens_per_second.toFixed(1)']) {
    if (dashboard.includes(staleNumberFormatting)) {
      throw new Error(`Dashboard still bypasses locale-aware number formatting: ${staleNumberFormatting}`);
    }
  }

  const modelList = readSource('src/components/ModelListPanel.tsx');
  if (!modelList.includes("t('list.readiness.withBackend'")) {
    throw new Error('Model readiness presentation must retain the concrete backend name.');
  }

  const effectiveSettings = readSource('src/components/EffectiveSettingsModal.tsx');
  if (!effectiveSettings.includes('type EffectiveNotice =')
      || effectiveSettings.includes('setNotice(t(')
      || effectiveSettings.includes('setNotice(friendlyErrorMessage(')
      || effectiveSettings.includes('setNotice(Object.keys(sampling)')) {
    throw new Error('Effective settings notices must remain semantic across locale changes.');
  }

  const downloadManager = readSource('src/components/DownloadManager.tsx');
  if (downloadManager.includes('}, [downloads, t]);')) {
    throw new Error('Download status transitions must not rerun just because the locale changed.');
  }
  if (downloadManager.includes("t('downloads.status')} {download.status}")
      || !downloadManager.includes('DOWNLOAD_STATUS_I18N_KEYS')) {
    throw new Error('Download detail status must be rendered through localized presentation keys.');
  }
  if (!downloadManager.includes("useI18n('backends')") || !downloadManager.includes('localizeDownloadError')) {
    throw new Error('Backend download errors must stay raw in store state and localize at presentation time.');
  }
  if (downloadManager.includes('bytes / Math.pow(k, i)).toFixed(2)')) {
    throw new Error('Download byte values must use locale-aware number formatting.');
  }

  const streamingLifecycle = readSource('src/hooks/useChatStreaming.ts');
  if (!streamingLifecycle.includes('recipes: compactBackendRecipes(anyData.recipes)')) {
    throw new Error('get_system_info history must retain compact recipe/backend detail.');
  }
  if (!streamingLifecycle.includes('localizedToolCapability')
      || !streamingLifecycle.includes('localizedBackendState')
      || streamingLifecycle.includes("String(backendInfo.state || '')")) {
    throw new Error('Known chat tool capabilities and backend states must be localized at presentation time.');
  }

  const streaming = readSource('src/hooks/useChatStreaming.ts');
  for (const stale of ['Too many tool call rounds', 'Tool execution failed:', 'Calling ${toolNames}', "'(stopped)'"]) {
    if (streaming.includes(stale)) throw new Error(`useChatStreaming contains stale English UI text: ${stale}`);
  }
  if (!streaming.includes('toolDisplayName(tc.function.name, t)')) {
    throw new Error('useChatStreaming must present known tool names through localized toolLabels.* keys.');
  }

  const capabilities = readSource('src/modelCapabilities.ts');
  if (/export function capabilityLabel\s*\(/.test(capabilities) || capabilities.includes('CAPABILITY_TAG_LABELS')) {
    throw new Error('modelCapabilities.ts still contains a second hard-coded capability presentation layer.');
  }

  const customModels = readSource('src/features/customModels/customModelStore.ts');
  if (/CUSTOM_CAPABILITIES:[\s\S]{0,500}\blabel\s*:/.test(customModels)) {
    throw new Error('CUSTOM_CAPABILITIES must contain stable values only; labels belong in locale catalogs.');
  }

  // OpenInference / OTel GenAI attribute names are protocol identifiers. They
  // intentionally remain raw so copied telemetry matches the standard schema.
  const overview = readSource('src/components/inspect/OverviewTab.tsx');
  for (const identifier of [
    'llm.model_name',
    'gen_ai.operation.name',
    'openinference.span.kind',
    'gen_ai.provider.name',
    'semantic.conventions',
    'llm.usage.total_tokens',
  ]) {
    if (!overview.includes(identifier)) {
      throw new Error(`Inspect overview no longer exposes telemetry identifier ${identifier}.`);
    }
  }
}
