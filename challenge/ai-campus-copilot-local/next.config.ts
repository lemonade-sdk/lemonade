import type { NextConfig } from "next";

/**
 * Two build modes.
 *
 * Default (local): a Next.js server runs, `/api/lemonade/*` proxies to Lemonade,
 * and LEMONADE_SERVER_URL stays server-side. This is the supported way to run
 * the app and what the README documents.
 *
 * Static (BUILD_STATIC_EXPORT=1): a hosted demo with no server. The proxy
 * cannot exist, so the browser calls Lemonade directly at a URL the user enters
 * on the Setup page. See docs/HOSTED_DEMO.md for the caveats.
 */
const isStaticExport = process.env["BUILD_STATIC_EXPORT"] === "1";

// GitHub Pages serves a project site under /<repo>, so assets need that prefix.
const basePath = process.env["PAGES_BASE_PATH"] ?? "";

const nextConfig: NextConfig = {
  reactStrictMode: true,
  poweredByHeader: false,
  eslint: {
    // `npm run lint` and `npm run check` run ESLint explicitly; running it again
    // inside `next build` doubles CI time for no extra signal.
    ignoreDuringBuilds: true,
  },

  ...(isStaticExport
    ? {
        output: "export" as const,
        images: { unoptimized: true },
        trailingSlash: true,
        ...(basePath ? { basePath, assetPrefix: basePath } : {}),
      }
    : {}),

  // `headers()` is unsupported under `output: "export"`; a static host sends its
  // own. GitHub Pages sets nosniff itself.
  ...(isStaticExport
    ? {}
    : {
        async headers() {
          return [
            {
              source: "/:path*",
              headers: [
                { key: "X-Content-Type-Options", value: "nosniff" },
                { key: "Referrer-Policy", value: "no-referrer" },
                { key: "X-Frame-Options", value: "DENY" },
              ],
            },
          ];
        },
      }),
};

export default nextConfig;
