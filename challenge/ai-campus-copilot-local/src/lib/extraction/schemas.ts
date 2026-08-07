import { z } from "zod";

export const DEADLINE_CATEGORIES = [
  "exam",
  "assignment",
  "application",
  "event",
  "fee",
  "scholarship",
  "interview",
  "other",
] as const;

export const CONFIDENCE_LEVELS = ["high", "medium", "low"] as const;

/**
 * ISO calendar date, or null when the source text is ambiguous.
 * Date.parse alone is not enough: it silently rolls 2027-02-31 over to 3 March,
 * which would turn a model hallucination into a plausible-looking deadline.
 */
const isoDate = z
  .string()
  .regex(/^\d{4}-\d{2}-\d{2}$/, "must be YYYY-MM-DD")
  .refine((value) => {
    const parsed = new Date(`${value}T00:00:00Z`);
    return !Number.isNaN(parsed.getTime()) && parsed.toISOString().slice(0, 10) === value;
  }, "must be a real calendar date");

export const deadlineSchema = z.object({
  title: z.string().min(1).max(300),
  dateText: z.string().min(1).max(300),
  normalizedDate: isoDate.nullable().catch(null),
  category: z.enum(DEADLINE_CATEGORIES).catch("other"),
  page: z.number().int().positive().nullable().catch(null),
  evidence: z.string().min(1).max(1200),
  confidence: z.enum(CONFIDENCE_LEVELS).catch("low"),
});

export const deadlineListSchema = z.array(deadlineSchema).max(120);

export type Deadline = z.infer<typeof deadlineSchema>;

export const EMPLOYMENT_TYPES = [
  "internship",
  "full-time",
  "part-time",
  "contract",
  "apprenticeship",
  "unspecified",
] as const;

export const opportunitySchema = z.object({
  organisation: z.string().min(1).max(200),
  role: z.string().min(1).max(200),
  employmentType: z.enum(EMPLOYMENT_TYPES).catch("unspecified"),
  eligibility: z.string().max(1000).nullable().catch(null),
  requiredSkills: z.array(z.string().max(120)).max(30).catch([]),
  location: z.string().max(200).nullable().catch(null),
  compensation: z.string().max(200).nullable().catch(null),
  applicationDeadline: z.string().max(200).nullable().catch(null),
  applicationLink: z.string().max(500).nullable().catch(null),
  page: z.number().int().positive().nullable().catch(null),
  evidence: z.string().min(1).max(1200),
  confidence: z.enum(CONFIDENCE_LEVELS).catch("low"),
});

export const opportunityListSchema = z.array(opportunitySchema).max(60);

export type Opportunity = z.infer<typeof opportunitySchema>;

export const SUMMARY_KINDS = [
  "short",
  "detailed",
  "keyPoints",
  "studentFriendly",
  "english",
  "tamil",
] as const;

export type SummaryKind = (typeof SUMMARY_KINDS)[number];

export interface SummaryRecord {
  kind: SummaryKind;
  text: string;
  model: string;
  createdAt: number;
  durationMs: number;
  /** True when the document was summarised in stages rather than one request. */
  hierarchical: boolean;
}

export type SummaryMap = Partial<Record<SummaryKind, SummaryRecord>>;

/**
 * Only http(s) links are surfaced as clickable. Anything else (javascript:,
 * data:, mailto: with tracking payloads) is rendered as plain text.
 */
export function safeHttpUrl(value: string | null): string | null {
  if (!value) return null;
  try {
    const url = new URL(value.trim());
    return url.protocol === "http:" || url.protocol === "https:" ? url.toString() : null;
  } catch {
    return null;
  }
}
