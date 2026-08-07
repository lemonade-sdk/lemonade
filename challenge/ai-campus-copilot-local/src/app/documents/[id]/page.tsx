import { DocumentDetail } from "@/components/documents/DocumentDetail";

/**
 * Document ids are minted at runtime and live only in the visitor's IndexedDB,
 * so there is no set of paths to pre-render. The static export therefore emits
 * none of these, and the hosted build links to /documents/view/?id= instead.
 */
export const dynamicParams = false;

export async function generateStaticParams() {
  return [];
}

export default async function DocumentDetailPage({
  params,
}: {
  params: Promise<{ id: string }>;
}) {
  const { id } = await params;
  return <DocumentDetail documentId={id} />;
}
