import type { Metadata, Viewport } from "next";
import type { ReactNode } from "react";
import { LemonadeProvider } from "@/components/providers/LemonadeProvider";
import { ThemeProvider } from "@/components/providers/ThemeProvider";
import { ToastProvider } from "@/components/providers/ToastProvider";
import { AppShell } from "@/components/shell/AppShell";
import "./globals.css";

const APP_NAME = process.env["NEXT_PUBLIC_APP_NAME"] ?? "AI Campus Copilot Local";

export const metadata: Metadata = {
  title: {
    default: APP_NAME,
    template: `%s — ${APP_NAME}`,
  },
  description:
    "A privacy-first student assistant that answers questions about college notices using a locally running Lemonade Server.",
  robots: { index: false, follow: false },
};

export const viewport: Viewport = {
  width: "device-width",
  initialScale: 1,
};

/**
 * Applies the stored theme before first paint so a dark-mode user never sees a
 * white flash. Content is a fixed literal, not user or model data.
 */
const THEME_BOOTSTRAP = `(function(){try{var t=localStorage.getItem("accl.theme")||"system";var d=t==="dark"||(t==="system"&&window.matchMedia("(prefers-color-scheme: dark)").matches);document.documentElement.classList.toggle("dark",d);}catch(e){}})();`;

export default function RootLayout({ children }: { children: ReactNode }) {
  return (
    <html lang="en" suppressHydrationWarning>
      <head>
        {/* eslint-disable-next-line react/no-danger */}
        <script dangerouslySetInnerHTML={{ __html: THEME_BOOTSTRAP }} />
      </head>
      <body>
        <ThemeProvider>
          <ToastProvider>
            <LemonadeProvider>
              <AppShell>{children}</AppShell>
            </LemonadeProvider>
          </ToastProvider>
        </ThemeProvider>
      </body>
    </html>
  );
}
