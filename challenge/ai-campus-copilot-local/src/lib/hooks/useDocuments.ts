"use client";

import { useCallback, useEffect, useState } from "react";
import { getSettings, listDocuments, saveSettings } from "@/lib/db/repo";
import type { StoredDocument } from "@/lib/documents/types";

export function useDocuments() {
  const [documents, setDocuments] = useState<StoredDocument[]>([]);
  const [activeDocumentId, setActiveDocumentIdState] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);

  const reload = useCallback(async () => {
    const [docs, settings] = await Promise.all([listDocuments(), getSettings()]);
    setDocuments(docs);

    // Fall back to the newest document when the stored selection is gone.
    const stored = settings.activeDocumentId;
    const valid = stored && docs.some((doc) => doc.id === stored) ? stored : (docs[0]?.id ?? null);
    setActiveDocumentIdState(valid);
    if (valid !== stored) await saveSettings({ activeDocumentId: valid });
    setLoading(false);
  }, []);

  useEffect(() => {
    void reload();
  }, [reload]);

  const setActiveDocumentId = useCallback(async (id: string | null) => {
    setActiveDocumentIdState(id);
    await saveSettings({ activeDocumentId: id });
  }, []);

  const activeDocument = documents.find((doc) => doc.id === activeDocumentId) ?? null;

  return { documents, activeDocument, activeDocumentId, setActiveDocumentId, loading, reload };
}
