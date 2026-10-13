import React, { useEffect, useMemo, useState } from 'react';
import type { IconName } from './Icon';
import {
  WorkspaceCatalogLayout,
  WorkspaceCatalogSection,
  type CatalogFilterDefinition,
} from './WorkspaceCatalogLayout';
import {
  WorkspaceActionButton,
  WorkspaceActionGroup,
  WorkspacePaneHeader,
} from './WorkspacePanels';
import { useI18n, type Locale } from '../i18n';

export type MarketplaceApp = {
  id: string;
  name: string;
  description?: string;
  localizedDescriptions?: Partial<Record<Locale, string>>;
  category?: string[];
  logo?: string;
  pinned?: boolean;
  links?: { app?: string; guide?: string; video?: string };
};

export type MarketplaceCategory = {
  id: string;
  label: string;
};

export const MARKETPLACE_URL = 'https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps.json';
const ALL_APPS_SECTION = 'all-apps';
const FEATURED_APP_LIMIT = 4;

const ZH_CN_APP_DESCRIPTIONS: Record<string, string> = {
  anythingllm: '使用本地设备模型的一体化 AI 生产力应用',
  'open-webui': '功能丰富的 Web 界面，用于在本地与 LLM 聊天',
  hermes: '具备自我改进能力的自主代理，支持持久化记忆和定时自动化',
  pi: '使用 Lemonade 本地模型的极简终端编程代理',
  'Claude Code': '代理式编程工具，可读取代码库、编辑文件、运行命令并集成开发工具',
  n8n: '原生集成 Lemonade 的工作流自动化平台，用于 AI 驱动的自动化',
  'github-copilot': '用于本地 AI 编程辅助的 VS Code Copilot 扩展',
  gaia: '用于设计本地优先代理的 SDK',
  dify: '构建基于节点的 AI 代理和 RAG 工作流',
  'fx-chatbot': '在 Firefox 浏览器中运行 Lemonade',
  'lemonade-mobile': '适用于自托管 Lemonade 服务器的 iOS 和 Android 聊天应用',
  'infinity-arcade': '生成并游玩无限的复古风格街机游戏重混作品',
  modelscope: '在 ModelScope 上浏览和发现 GGUF 模型，可直接从 Lemonade 下载',
  openclaw: '能够编写和运行代码、管理文件并完成多步骤任务的自主代理',
  listenr: '完全在本地录音、转写并微调自己的音频模型，由 Lemonade 提供支持',
  'dream-server': '私有本地 AI 服务器，支持聊天、代理、RAG、工作流和自托管应用，并由 Lemonade 提供 AMD 推理支持',
  'lemon-zest': '使用本地 Stable Diffusion 模型的开源图像编辑器',
  interviewer: '支持语音模拟面试的 AI 面试练习应用',
  codegpt: '支持本地 LLM 的 VS Code AI 编程助手',
  'deep-tutor': '提供个性化学习体验的 AI 辅导平台',
  'hugging-face': '在 Hugging Face Hub 上浏览和发现兼容 Lemonade 的模型',
  'iterate-ai': '用于构建和部署 AI 解决方案的企业级 AI 平台',
  'ai-toolkit': '在 VS Code 扩展中大规模试验 LLM',
  morphik: '集中管理业务知识，构建可靠的 AI 代理来自动化处理任务',
  openhands: '能够编写代码、修复错误等的 AI 软件开发代理',
  vane: '支持本地 LLM 的开源 AI 搜索引擎',
};

function categorySectionId(categoryKey: string): string {
  const safeCategory = encodeURIComponent(categoryKey)
    .replace(/%/g, '')
    .replace(/[^a-z0-9_-]+/g, '-');
  return `category-${safeCategory || 'other'}`;
}

function categoryIcon(category: string): IconName {
  const normalized = category.toLowerCase();
  if (/chat|assistant|conversation|bot/.test(normalized)) return 'chat';
  if (/image|photo|design|creative|art/.test(normalized)) return 'image';
  if (/audio|music|speech|voice/.test(normalized)) return 'audio';
  if (/code|developer|development|ide/.test(normalized)) return 'code';
  if (/search|research|knowledge|rag/.test(normalized)) return 'search';
  if (/automation|workflow|tool|productivity/.test(normalized)) return 'tools';
  if (/3d|model/.test(normalized)) return 'box';
  return 'layers';
}

function appCountLabel(count: number): string {
  return `${count} compatible ${count === 1 ? 'app' : 'apps'}`;
}

function categoryLabel(category: string): string {
  return category.trim().replace(/(^|[\s/&-])(\p{Ll})/gu, (_match, separator: string, letter: string) =>
    `${separator}${letter.toLocaleUpperCase()}`);
}

function normalizedCategory(category: string): string {
  return category.trim().toLocaleLowerCase();
}

type CategoryGroup = {
  key: string;
  label: string;
  apps: MarketplaceApp[];
};

function collectCategoryGroups(
  apps: MarketplaceApp[],
  categoriesOf: (app: MarketplaceApp) => string[],
  labelByCategory: Map<string, string>,
  feedOrder: Map<string, number>,
): CategoryGroup[] {
  const groups = new Map<string, CategoryGroup>();
  apps.forEach(app => {
    const rawCategories = categoriesOf(app).map(category => category.trim());
    const seenKeys = new Set<string>();
    (rawCategories.length ? rawCategories : ['']).forEach(raw => {
      const key = raw ? normalizedCategory(raw) : 'other';
      if (seenKeys.has(key)) return;
      seenKeys.add(key);
      let group = groups.get(key);
      if (!group) {
        group = { key, label: raw ? labelByCategory.get(key) ?? categoryLabel(raw) : 'Other', apps: [] };
        groups.set(key, group);
      }
      group.apps.push(app);
    });
  });
  return Array.from(groups.values())
    .map(group => ({ ...group, apps: [...group.apps].sort((a, b) => a.name.localeCompare(b.name)) }))
    .sort((left, right) => {
      const leftOrder = feedOrder.get(left.key);
      const rightOrder = feedOrder.get(right.key);
      if (leftOrder !== undefined && rightOrder !== undefined) return leftOrder - rightOrder;
      if (leftOrder !== undefined) return -1;
      if (rightOrder !== undefined) return 1;
      return left.label.localeCompare(right.label);
    });
}

/* Dark artwork over a transparent field disappears on the dark theme, so
 * those logos get a white tile behind them; opaque or bright logos render
 * bare. Sampled through a canvas, which requires CORS-enabled logo hosts. */
const logoTileCache = new Map<string, boolean>();

function logoNeedsTile(src: string): Promise<boolean> {
  return new Promise(resolve => {
    const probe = new Image();
    probe.crossOrigin = 'anonymous';
    probe.onload = () => {
      try {
        const size = 24;
        const canvas = document.createElement('canvas');
        canvas.width = size;
        canvas.height = size;
        const context = canvas.getContext('2d');
        if (!context) {
          resolve(false);
          return;
        }
        context.drawImage(probe, 0, 0, size, size);
        const { data } = context.getImageData(0, 0, size, size);
        let transparent = 0;
        let opaque = 0;
        let luminanceSum = 0;
        for (let i = 0; i < data.length; i += 4) {
          if (data[i + 3] < 26) {
            transparent += 1;
            continue;
          }
          opaque += 1;
          luminanceSum += (0.299 * data[i] + 0.587 * data[i + 1] + 0.114 * data[i + 2]) / 255;
        }
        const transparentShare = transparent / (transparent + opaque || 1);
        const meanLuminance = opaque ? luminanceSum / opaque : 1;
        resolve(transparentShare > 0.05 && meanLuminance < 0.45);
      } catch {
        resolve(false);
      }
    };
    probe.onerror = () => resolve(false);
    probe.src = src;
  });
}

const AppLogo: React.FC<{ src: string }> = ({ src }) => {
  const [tiled, setTiled] = useState(() => logoTileCache.get(src) ?? false);

  useEffect(() => {
    if (logoTileCache.has(src)) {
      setTiled(logoTileCache.get(src)!);
      return undefined;
    }
    let cancelled = false;
    logoNeedsTile(src).then(needsTile => {
      logoTileCache.set(src, needsTile);
      if (!cancelled) setTiled(needsTile);
    });
    return () => { cancelled = true; };
  }, [src]);

  return <img className={`app-card__logo${tiled ? ' app-card__logo--tile' : ''}`} src={src} alt="" />;
};

type AppsViewProps = {
  apps: MarketplaceApp[];
  categories: MarketplaceCategory[];
  loading: boolean;
  error: string | null;
};

const AppsView: React.FC<AppsViewProps> = ({
  apps: marketplaceApps,
  categories: marketplaceCategories,
  loading: marketplaceLoading,
  error: marketplaceError,
}) => {
  const { locale, t } = useI18n();
  const [categoryFilter, setCategoryFilter] = useState<string | null>(null);

  const featuredApps = useMemo(
    () => marketplaceApps.filter(app => app.pinned).slice(0, FEATURED_APP_LIMIT),
    [marketplaceApps],
  );

  const labelByCategory = useMemo(() => new Map(
    marketplaceCategories.map(category => [normalizedCategory(category.id), category.label]),
  ), [marketplaceCategories]);

  const displayLabel = (rawCategory: string): string =>
    labelByCategory.get(normalizedCategory(rawCategory)) ?? categoryLabel(rawCategory);

  const feedOrder = useMemo(() => new Map(
    marketplaceCategories.map((category, index) => [normalizedCategory(category.id), index]),
  ), [marketplaceCategories]);

  /* Sections group each app under its primary category; the sidebar filters
   * match every assigned category, so a multi-category app is reachable from
   * all of them. */
  const categoryGroups = useMemo(
    () => collectCategoryGroups(marketplaceApps, app => app.category?.slice(0, 1) ?? [], labelByCategory, feedOrder),
    [feedOrder, labelByCategory, marketplaceApps],
  );

  const categoryOptions = useMemo(
    () => collectCategoryGroups(marketplaceApps, app => app.category ?? [], labelByCategory, feedOrder),
    [feedOrder, labelByCategory, marketplaceApps],
  );

  useEffect(() => {
    if (categoryFilter && !categoryOptions.some(group => group.key === categoryFilter)) setCategoryFilter(null);
  }, [categoryOptions, categoryFilter]);

  const categoryFilters = useMemo<CatalogFilterDefinition<string>[]>(() => [
    {
      id: ALL_APPS_SECTION,
      label: t('All Apps'),
      description: marketplaceLoading
        ? t('Loading directory')
        : marketplaceError
          ? t('Directory unavailable')
          : t('Compatible clients and tools'),
      icon: 'globe',
      count: marketplaceLoading || marketplaceError ? undefined : marketplaceApps.length,
    },
    ...categoryOptions.map(group => ({
      id: categorySectionId(group.key),
      label: t(group.label),
      description: t('{count} compatible apps', { count: group.apps.length }),
      icon: categoryIcon(group.key),
      count: group.apps.length,
    })),
  ], [categoryOptions, marketplaceApps.length, marketplaceError, marketplaceLoading, t]);

  const categoryBySection = useMemo(() => new Map(
    categoryOptions.map(group => [categorySectionId(group.key), group.key]),
  ), [categoryOptions]);

  const activeCategorySection = categoryFilter ? categorySectionId(categoryFilter) : ALL_APPS_SECTION;
  const activeGroup = categoryFilter
    ? categoryOptions.find(group => group.key === categoryFilter) ?? null
    : null;

  const openExternal = (url?: string) => {
    if (!url) return;
    const hostApi = (window as unknown as { api?: { openExternal?: (url: string) => void } }).api;
    if (hostApi?.openExternal) {
      hostApi.openExternal(url);
      return;
    }
    window.open(url, '_blank', 'noopener,noreferrer');
  };

  const paneTitle = t('Apps Marketplace');
  const paneSubtitle = t('Lemonade works best as the inference server for applications. Try out this curated list of apps!');

  const showFeatured = !activeGroup && featuredApps.length > 0;
  const visibleGroups = (activeGroup ? [activeGroup] : categoryGroups)
    .map(group => ({
      ...group,
      apps: showFeatured ? group.apps.filter(app => !featuredApps.includes(app)) : group.apps,
    }))
    .filter(group => group.apps.length > 0);

  const renderAppCard = (app: MarketplaceApp) => (
    <article key={app.id || app.name} className="workspace-card app-card">
      <header className="workspace-card__head">
        {app.logo
          ? <AppLogo src={app.logo} />
          : <span className="app-card__logo app-card__logo--tile app-card__logo--fallback" aria-hidden="true">{app.name.slice(0, 1).toUpperCase()}</span>}
        <span className="app-card__identity">
          <h3 className="workspace-card__name app-card__name">{app.name}</h3>
          {app.category && app.category.length > 0 && (
            <span className="app-card__category">{app.category.map(category => t(displayLabel(category))).join(' · ')}</span>
          )}
        </span>
      </header>
      <p className="app-card__description">
        {app.localizedDescriptions?.[locale]
          ?? (locale === 'zh-CN' ? ZH_CN_APP_DESCRIPTIONS[app.id] : undefined)
          ?? app.description
          ?? t('No description available.')}
      </p>
      <footer className="app-card__footer">
        <WorkspaceActionGroup className="app-card__actions" label={t('Links for {name}', { name: app.name })}>
          {app.links?.app && <WorkspaceActionButton appearance="secondary" size="small" icon="globe" onClick={() => openExternal(app.links?.app)}>{t('Visit')}</WorkspaceActionButton>}
          {app.links?.guide && <WorkspaceActionButton appearance="quiet" size="small" onClick={() => openExternal(app.links?.guide)}>{t('Guide')}</WorkspaceActionButton>}
          {app.links?.video && <WorkspaceActionButton appearance="quiet" size="small" onClick={() => openExternal(app.links?.video)}>{t('Video')}</WorkspaceActionButton>}
        </WorkspaceActionGroup>
      </footer>
    </article>
  );

  return (
    <WorkspaceCatalogLayout
      view="apps"
      className="apps-workspace"
      panelId="apps-types-panel"
      railTitle={t('Filters')}
      railLabel={t('App categories')}
      sidebarLabel={t('app categories')}
      mobileMenuLabel={t('Open app categories')}
      filters={categoryFilters}
      activeFilter={activeCategorySection}
      onFilterChange={section => setCategoryFilter(categoryBySection.get(section) ?? null)}
      header={
        <WorkspacePaneHeader
          headingLevel={1}
          title={paneTitle}
          subtitle={paneSubtitle}
          titleId="apps-pane-title"
        />
      }
    >
      {marketplaceLoading ? (
        <div className="apps__empty" role="status">{t('Loading apps...')}</div>
      ) : marketplaceError ? (
        <div className="apps__error" role="alert">{t('Apps unavailable: {error}', { error: marketplaceError })}</div>
      ) : (
        <div className="workspace-catalog" aria-label={`${paneTitle} directory`}>
          {showFeatured && (
            <WorkspaceCatalogSection
              title={t('Featured')}
              description={t('Picks from the Lemonade team.')}
            >
              {featuredApps.map(renderAppCard)}
            </WorkspaceCatalogSection>
          )}
          {visibleGroups.map(group => (
            <WorkspaceCatalogSection key={group.key} title={t(group.label)}>
              {group.apps.map(renderAppCard)}
            </WorkspaceCatalogSection>
          ))}
          {marketplaceApps.length === 0 && (
            <div className="apps__empty">
              {t('No apps are available yet.')}
            </div>
          )}
        </div>
      )}
    </WorkspaceCatalogLayout>
  );
};

export default AppsView;
