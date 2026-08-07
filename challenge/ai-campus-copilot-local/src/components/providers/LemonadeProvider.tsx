"use client";

import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { createBrowserClient, type LemonadeClient } from "@/lib/lemonade/client";
import { isLemonadeError } from "@/lib/lemonade/errors";
import {
  chatModels,
  classifyModels,
  embeddingModels,
  type ClassifiedModel,
} from "@/lib/lemonade/models";
import type { LemonadeHealth, LemonadeSystemInfo } from "@/lib/lemonade/schemas";
import { getSettings, saveSettings } from "@/lib/db/repo";

/**
 * Connection status is derived strictly from the last observed request outcome.
 * There is no optimistic "connected" state — the UI only ever shows what was
 * actually measured.
 */
export type ConnectionStatus =
  | "checking"
  | "connected"
  | "server_unavailable"
  | "no_models_installed"
  | "request_failed";

export interface LemonadeContextValue {
  status: ConnectionStatus;
  /** Human-readable explanation of the current status. */
  statusDetail: string | null;
  health: LemonadeHealth | null;
  systemInfo: LemonadeSystemInfo | null;
  allModels: ClassifiedModel[];
  availableChatModels: ClassifiedModel[];
  availableEmbeddingModels: ClassifiedModel[];
  chatModel: string | null;
  embeddingModel: string | null;
  setChatModel: (id: string | null) => Promise<void>;
  setEmbeddingModel: (id: string | null) => Promise<void>;
  refresh: () => Promise<void>;
  isRefreshing: boolean;
  /** Number of in-flight requests to Lemonade, for the network indicator. */
  activeRequests: number;
  beginRequest: () => () => void;
  client: LemonadeClient;
  settingsLoaded: boolean;
  /** True only when the server responded AND both models are chosen. */
  isReady: boolean;
}

const LemonadeContext = createContext<LemonadeContextValue | null>(null);

export function LemonadeProvider({ children }: { children: ReactNode }) {
  const client = useMemo(() => createBrowserClient(), []);
  const [status, setStatus] = useState<ConnectionStatus>("checking");
  const [statusDetail, setStatusDetail] = useState<string | null>(null);
  const [health, setHealth] = useState<LemonadeHealth | null>(null);
  const [systemInfo, setSystemInfo] = useState<LemonadeSystemInfo | null>(null);
  const [allModels, setAllModels] = useState<ClassifiedModel[]>([]);
  const [chatModel, setChatModelState] = useState<string | null>(null);
  const [embeddingModel, setEmbeddingModelState] = useState<string | null>(null);
  const [settingsLoaded, setSettingsLoaded] = useState(false);
  const [isRefreshing, setIsRefreshing] = useState(false);
  const [activeRequests, setActiveRequests] = useState(0);

  const mounted = useRef(true);
  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
    };
  }, []);

  const beginRequest = useCallback(() => {
    setActiveRequests((count) => count + 1);
    let settled = false;
    return () => {
      if (settled) return;
      settled = true;
      setActiveRequests((count) => Math.max(0, count - 1));
    };
  }, []);

  useEffect(() => {
    void (async () => {
      try {
        const settings = await getSettings();
        if (!mounted.current) return;
        setChatModelState(settings.chatModel);
        setEmbeddingModelState(settings.embeddingModel);
      } finally {
        if (mounted.current) setSettingsLoaded(true);
      }
    })();
  }, []);

  const refresh = useCallback(async () => {
    setIsRefreshing(true);
    setStatus((current) => (current === "checking" ? current : "checking"));
    const endRequest = beginRequest();

    try {
      const healthResult = await client.health();
      if (!mounted.current) return;
      setHealth(healthResult.value);

      const modelsResult = await client.listModels();
      if (!mounted.current) return;
      const classified = classifyModels(modelsResult.value);
      setAllModels(classified);

      if (classified.length === 0) {
        setStatus("no_models_installed");
        setStatusDetail(
          "Lemonade Server is running but has no downloaded models. Use `lemonade pull <model>` or the Model Manager.",
        );
      } else {
        setStatus("connected");
        setStatusDetail(null);
      }

      // System info is a nice-to-have; a failure here must not flip the
      // connection status, which health + models already established.
      try {
        const info = await client.systemInfo();
        if (mounted.current) setSystemInfo(info.value);
      } catch {
        if (mounted.current) setSystemInfo(null);
      }
    } catch (error) {
      if (!mounted.current) return;
      setHealth(null);
      setAllModels([]);
      if (isLemonadeError(error)) {
        setStatus(error.kind === "server_unavailable" ? "server_unavailable" : "request_failed");
        setStatusDetail(error.displayMessage);
      } else {
        setStatus("request_failed");
        setStatusDetail(error instanceof Error ? error.message : "Unknown error");
      }
    } finally {
      endRequest();
      if (mounted.current) setIsRefreshing(false);
    }
  }, [client, beginRequest]);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const setChatModel = useCallback(async (id: string | null) => {
    setChatModelState(id);
    await saveSettings({ chatModel: id });
  }, []);

  const setEmbeddingModel = useCallback(async (id: string | null) => {
    setEmbeddingModelState(id);
    await saveSettings({ embeddingModel: id });
  }, []);

  const availableChatModels = useMemo(() => chatModels(allModels), [allModels]);
  const availableEmbeddingModels = useMemo(() => embeddingModels(allModels), [allModels]);

  // A stored selection that is no longer installed must not read as "ready".
  const chatModelInstalled = chatModel !== null && allModels.some((m) => m.id === chatModel);
  const embeddingModelInstalled =
    embeddingModel !== null && allModels.some((m) => m.id === embeddingModel);

  const value = useMemo<LemonadeContextValue>(
    () => ({
      status,
      statusDetail,
      health,
      systemInfo,
      allModels,
      availableChatModels,
      availableEmbeddingModels,
      chatModel,
      embeddingModel,
      setChatModel,
      setEmbeddingModel,
      refresh,
      isRefreshing,
      activeRequests,
      beginRequest,
      client,
      settingsLoaded,
      isReady: status === "connected" && chatModelInstalled && embeddingModelInstalled,
    }),
    [
      status,
      statusDetail,
      health,
      systemInfo,
      allModels,
      availableChatModels,
      availableEmbeddingModels,
      chatModel,
      embeddingModel,
      setChatModel,
      setEmbeddingModel,
      refresh,
      isRefreshing,
      activeRequests,
      beginRequest,
      client,
      settingsLoaded,
      chatModelInstalled,
      embeddingModelInstalled,
    ],
  );

  return <LemonadeContext.Provider value={value}>{children}</LemonadeContext.Provider>;
}

export function useLemonade(): LemonadeContextValue {
  const context = useContext(LemonadeContext);
  if (!context) throw new Error("useLemonade must be used inside LemonadeProvider");
  return context;
}
