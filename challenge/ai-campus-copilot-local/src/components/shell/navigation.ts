import {
  Briefcase,
  CalendarClock,
  FileText,
  Gauge,
  Info,
  LayoutDashboard,
  MessagesSquare,
  ScrollText,
  ShieldCheck,
  PlugZap,
  type LucideIcon,
} from "lucide-react";

export interface NavItem {
  href: string;
  label: string;
  icon: LucideIcon;
  group: "workspace" | "insights" | "about";
}

export const NAV_ITEMS: NavItem[] = [
  { href: "/dashboard", label: "Dashboard", icon: LayoutDashboard, group: "workspace" },
  { href: "/documents", label: "Documents", icon: FileText, group: "workspace" },
  { href: "/chat", label: "Ask", icon: MessagesSquare, group: "workspace" },
  { href: "/summaries", label: "Summaries", icon: ScrollText, group: "insights" },
  { href: "/deadlines", label: "Deadlines", icon: CalendarClock, group: "insights" },
  { href: "/careers", label: "Careers", icon: Briefcase, group: "insights" },
  { href: "/performance", label: "Performance", icon: Gauge, group: "insights" },
  { href: "/setup", label: "Setup", icon: PlugZap, group: "about" },
  { href: "/privacy", label: "Privacy", icon: ShieldCheck, group: "about" },
  { href: "/about", label: "About", icon: Info, group: "about" },
];

export const GROUP_LABELS: Record<NavItem["group"], string> = {
  workspace: "Workspace",
  insights: "Insights",
  about: "Setup & info",
};

/** Items shown in the mobile bottom bar, where space is tight. */
export const MOBILE_NAV_HREFS = ["/dashboard", "/documents", "/chat", "/deadlines", "/setup"];
