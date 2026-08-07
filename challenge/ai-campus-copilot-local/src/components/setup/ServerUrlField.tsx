"use client";

import { Save } from "lucide-react";
import { useEffect, useId, useState } from "react";
import { useLemonade } from "@/components/providers/LemonadeProvider";
import { useToast } from "@/components/providers/ToastProvider";
import { Card } from "@/components/ui/Primitives";

/**
 * Only rendered by the hosted static build. The local build reads the address
 * from LEMONADE_SERVER_URL on the server, where the browser cannot see it.
 */
export function ServerUrlField() {
  const { serverUrl, setServerUrl, refresh, isDirectMode } = useLemonade();
  const toast = useToast();
  const inputId = useId();
  const [draft, setDraft] = useState(serverUrl ?? "");

  useEffect(() => {
    if (serverUrl) setDraft(serverUrl);
  }, [serverUrl]);

  if (!isDirectMode) return null;

  const submit = () => {
    if (!setServerUrl(draft)) {
      toast.error("Enter a valid http:// or https:// address, for example http://localhost:13305");
      return;
    }
    toast.success("Server address saved. Rechecking…");
    void refresh();
  };

  return (
    <Card>
      <label htmlFor={inputId} className="label">
        Lemonade Server address
      </label>
      <div className="flex flex-wrap gap-2">
        <input
          id={inputId}
          type="url"
          inputMode="url"
          className="field flex-1"
          value={draft}
          placeholder="http://localhost:13305"
          onChange={(event) => setDraft(event.target.value)}
          onKeyDown={(event) => {
            if (event.key === "Enter") {
              event.preventDefault();
              submit();
            }
          }}
        />
        <button type="button" className="btn-primary shrink-0" onClick={submit}>
          <Save className="h-4 w-4" aria-hidden />
          Save
        </button>
      </div>
      <p className="mt-2 text-xs text-ink-muted">
        This hosted page has no server of its own, so your browser calls Lemonade directly. The
        address is stored in this browser only and is never sent anywhere.
      </p>
    </Card>
  );
}
