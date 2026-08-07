"use client";

import { GraduationCap, Menu, Moon, Sun, X } from "lucide-react";
import Link from "next/link";
import { usePathname } from "next/navigation";
import { useEffect, useState, type ReactNode } from "react";
import { useTheme } from "@/components/providers/ThemeProvider";
import { ErrorBoundary } from "@/components/ui/ErrorBoundary";
import { cn } from "@/lib/utils/cn";
import { GROUP_LABELS, MOBILE_NAV_HREFS, NAV_ITEMS } from "./navigation";
import { LocalAiBadge, NetworkActivityIndicator, StatusIndicator } from "./StatusIndicator";

const APP_NAME = process.env["NEXT_PUBLIC_APP_NAME"] ?? "AI Campus Copilot Local";

export function AppShell({ children }: { children: ReactNode }) {
  const pathname = usePathname();
  const [mobileNavOpen, setMobileNavOpen] = useState(false);

  useEffect(() => setMobileNavOpen(false), [pathname]);

  return (
    <div className="min-h-dvh lg:grid lg:grid-cols-[16rem_1fr]">
      <a
        href="#main"
        className="sr-only focus:not-sr-only focus:absolute focus:left-3 focus:top-3 focus:z-50 focus:rounded-lg focus:bg-brand focus:px-3 focus:py-2 focus:text-white"
      >
        Skip to main content
      </a>

      <Sidebar pathname={pathname} />

      <div className="flex min-w-0 flex-col">
        <TopBar onOpenNav={() => setMobileNavOpen(true)} />
        <main id="main" className="flex-1 px-4 pb-24 pt-6 lg:px-8 lg:pb-10">
          <div className="mx-auto w-full max-w-5xl">
            <ErrorBoundary>{children}</ErrorBoundary>
          </div>
        </main>
        <Footer />
      </div>

      <MobileNav open={mobileNavOpen} onClose={() => setMobileNavOpen(false)} pathname={pathname} />
      <MobileTabBar pathname={pathname} />
    </div>
  );
}

function Sidebar({ pathname }: { pathname: string }) {
  return (
    <aside className="sticky top-0 hidden h-dvh flex-col border-r border-line bg-surface-raised lg:flex">
      <Link href="/" className="flex items-center gap-2.5 border-b border-line px-5 py-4">
        <GraduationCap className="h-6 w-6 text-brand" aria-hidden />
        <span className="text-sm font-semibold leading-tight text-ink">{APP_NAME}</span>
      </Link>
      <nav className="flex-1 overflow-y-auto p-3" aria-label="Main">
        <NavList pathname={pathname} />
      </nav>
      <div className="border-t border-line p-3">
        <LocalAiBadge />
      </div>
    </aside>
  );
}

function NavList({ pathname }: { pathname: string }) {
  const groups = ["workspace", "insights", "about"] as const;

  return (
    <div className="flex flex-col gap-5">
      {groups.map((group) => (
        <div key={group}>
          <p className="px-3 pb-1.5 text-xs font-semibold uppercase tracking-wide text-ink-faint">
            {GROUP_LABELS[group]}
          </p>
          <ul className="flex flex-col gap-0.5">
            {NAV_ITEMS.filter((item) => item.group === group).map((item) => {
              const Icon = item.icon;
              const active = pathname === item.href || pathname.startsWith(`${item.href}/`);
              return (
                <li key={item.href}>
                  <Link
                    href={item.href}
                    aria-current={active ? "page" : undefined}
                    className={cn(
                      "flex items-center gap-2.5 rounded-lg px-3 py-2 text-sm transition-colors",
                      active
                        ? "bg-brand-soft font-medium text-brand"
                        : "text-ink-muted hover:bg-surface-sunken hover:text-ink",
                    )}
                  >
                    <Icon className="h-4 w-4 shrink-0" aria-hidden />
                    {item.label}
                  </Link>
                </li>
              );
            })}
          </ul>
        </div>
      ))}
    </div>
  );
}

function TopBar({ onOpenNav }: { onOpenNav: () => void }) {
  return (
    <header className="sticky top-0 z-30 flex items-center gap-2 border-b border-line bg-surface/85 px-4 py-3 backdrop-blur lg:px-8">
      <button
        type="button"
        onClick={onOpenNav}
        className="btn-ghost px-2 lg:hidden"
        aria-label="Open navigation menu"
      >
        <Menu className="h-5 w-5" aria-hidden />
      </button>
      <Link href="/" className="flex items-center gap-2 lg:hidden">
        <GraduationCap className="h-5 w-5 text-brand" aria-hidden />
        <span className="text-sm font-semibold text-ink">Campus Copilot</span>
      </Link>
      <div className="ml-auto flex items-center gap-2">
        <NetworkActivityIndicator />
        <StatusIndicator />
        <ThemeToggle />
      </div>
    </header>
  );
}

function ThemeToggle() {
  const { resolved, setTheme } = useTheme();
  const next = resolved === "dark" ? "light" : "dark";

  return (
    <button
      type="button"
      onClick={() => setTheme(next)}
      className="btn-ghost px-2"
      aria-label={`Switch to ${next} mode`}
    >
      {resolved === "dark" ? (
        <Sun className="h-4 w-4" aria-hidden />
      ) : (
        <Moon className="h-4 w-4" aria-hidden />
      )}
    </button>
  );
}

function MobileNav({
  open,
  onClose,
  pathname,
}: {
  open: boolean;
  onClose: () => void;
  pathname: string;
}) {
  if (!open) return null;

  return (
    <div className="fixed inset-0 z-40 lg:hidden">
      <button
        type="button"
        className="absolute inset-0 bg-black/50"
        onClick={onClose}
        aria-label="Close navigation menu"
      />
      <div className="absolute left-0 top-0 h-full w-72 max-w-[85vw] overflow-y-auto border-r border-line bg-surface-raised p-3">
        <div className="mb-3 flex items-center justify-between px-2">
          <span className="text-sm font-semibold text-ink">{APP_NAME}</span>
          <button
            type="button"
            onClick={onClose}
            className="btn-ghost px-2"
            aria-label="Close navigation menu"
          >
            <X className="h-4 w-4" aria-hidden />
          </button>
        </div>
        <nav aria-label="Mobile">
          <NavList pathname={pathname} />
        </nav>
      </div>
    </div>
  );
}

function MobileTabBar({ pathname }: { pathname: string }) {
  const items = NAV_ITEMS.filter((item) => MOBILE_NAV_HREFS.includes(item.href));

  return (
    <nav
      className="fixed bottom-0 left-0 right-0 z-30 flex border-t border-line bg-surface-raised lg:hidden"
      aria-label="Quick navigation"
    >
      {items.map((item) => {
        const Icon = item.icon;
        const active = pathname === item.href || pathname.startsWith(`${item.href}/`);
        return (
          <Link
            key={item.href}
            href={item.href}
            aria-current={active ? "page" : undefined}
            className={cn(
              "flex flex-1 flex-col items-center gap-1 py-2.5 text-[11px]",
              active ? "text-brand" : "text-ink-faint",
            )}
          >
            <Icon className="h-5 w-5" aria-hidden />
            {item.label}
          </Link>
        );
      })}
    </nav>
  );
}

function Footer() {
  return (
    <footer className="hidden border-t border-line px-8 py-4 text-xs text-ink-faint lg:block">
      <p>
        {APP_NAME} — created by Jerome Prakash L for the AMD Lemonade Developer Challenge. Powered
        by Lemonade local AI. Apache-2.0 licensed.
      </p>
    </footer>
  );
}
