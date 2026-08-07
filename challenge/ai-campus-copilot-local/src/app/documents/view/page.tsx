"use client";

import { useSearchParams } from "next/navigation";
import { Suspense } from "react";
import { DocumentDetail } from "@/components/documents/DocumentDetail";
import { SkeletonList } from "@/components/ui/Primitives";

/**
 * Query-param twin of /documents/[id]. A single static page can serve every
 * document, which a dynamic segment cannot do under `output: "export"`.
 */
function ViewByQuery() {
  const documentId = useSearchParams().get("id") ?? "";
  return <DocumentDetail documentId={documentId} />;
}

export default function DocumentViewPage() {
  return (
    <Suspense fallback={<SkeletonList rows={3} />}>
      <ViewByQuery />
    </Suspense>
  );
}
