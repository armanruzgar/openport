import type { ReactNode } from "react"
import type { FeedState } from "../api/types"
import { groupKeyIndex } from "../lib/keyboard"

export function Panel({ title, actions, children, className = "" }: { title?: ReactNode; actions?: ReactNode; children: ReactNode; className?: string }) {
  return (
    <section className={`min-w-0 rounded-lg border border-border bg-panel ${className}`}>
      {(title || actions) && (
        <header className="flex flex-wrap items-center justify-between gap-2 border-b border-border px-3 py-2">
          <h2 className="text-xs font-medium uppercase tracking-wide text-muted">{title}</h2>
          {actions && <div className="flex flex-wrap items-center gap-2">{actions}</div>}
        </header>
      )}
      <div className="p-3">{children}</div>
    </section>
  )
}

export function Stat({ label, value, hint, tone }: { label: string; value: ReactNode; hint?: string; tone?: "positive" | "negative" | "warn" }) {
  const colour = tone === "positive" ? "text-gex-positive" : tone === "negative" ? "text-gex-negative" : tone === "warn" ? "text-warn" : ""
  return (
    <div className="rounded-lg border border-border bg-panel px-3 py-2" title={hint}>
      <div className="text-[11px] uppercase tracking-wide text-muted">{label}</div>
      <div className={`mt-0.5 text-lg tabular ${colour}`}>{value}</div>
    </div>
  )
}

/// A row of mutually exclusive options.
export function Segmented<T extends string | number>({
  value,
  options,
  onChange,
  label,
}: {
  value: T
  options: { value: T; label: string }[]
  onChange: (value: T) => void
  label: string
}) {
  return (
    <div role="radiogroup" aria-label={label} className="inline-flex rounded-md border border-border bg-background p-0.5">
      {options.map((o, index) => (
        <button
          key={String(o.value)}
          type="button"
          role="radio"
          aria-checked={o.value === value}
          tabIndex={o.value === value ? 0 : -1}
          onClick={() => onChange(o.value)}
          onKeyDown={(event) => {
            if (event.altKey || event.ctrlKey || event.metaKey) return
            const next = groupKeyIndex(event.key, index, options.length)
            if (next == null) return
            const option = options[next]
            if (!option) return
            event.preventDefault()
            event.stopPropagation()
            onChange(option.value)
            event.currentTarget.parentElement?.querySelectorAll<HTMLButtonElement>('[role="radio"]').item(next)?.focus()
          }}
          className={`rounded px-2 py-0.5 text-xs transition-colors ${
            o.value === value ? "bg-raised text-foreground" : "text-muted hover:text-foreground"
          }`}
        >
          {o.label}
        </button>
      ))}
    </div>
  )
}

const feedTone: Record<FeedState, string> = {
  live: "bg-live",
  delayed: "bg-stale",
  connecting: "bg-warn",
  stale: "bg-warn",
  error: "bg-danger",
  stopped: "bg-faint",
}

/** A feed's state; `label` names a state of the terminal's own connection instead. */
export function FeedBadge({ state, message, label }: { state: FeedState; message?: string | null; label?: string }) {
  return (
    <span className="inline-flex items-center gap-1.5 rounded-full border border-border px-2 py-0.5 text-[11px]" title={message ?? undefined}>
      <span className={`h-1.5 w-1.5 rounded-full ${feedTone[state]} ${state === "live" ? "animate-pulse" : ""}`} />
      {label ?? state}
    </span>
  )
}

export function Empty({ children }: { children: ReactNode }) {
  return <div className="flex min-h-40 items-center justify-center text-sm text-muted">{children}</div>
}

export type Tone = "positive" | "negative" | "warn" | "neutral" | "accent"
const badgeTone: Record<Tone, string> = {
  positive: "border-bullish/40 bg-bullish/10 text-bullish",
  negative: "border-bearish/40 bg-bearish/10 text-bearish",
  warn: "border-warn/40 bg-warn/10 text-warn",
  neutral: "border-border bg-raised text-muted",
  accent: "border-accent/40 bg-accent/10 text-accent",
}
export function Badge({ tone = "neutral", children, title }: { tone?: Tone; children: ReactNode; title?: string }) {
  return <span title={title} className={`inline-flex items-center gap-1 rounded-full border px-2 py-0.5 text-[10px] font-medium uppercase tracking-wide ${badgeTone[tone]}`}>{children}</span>
}

/** Sign-aware text colour for P&L values. */
export function toneOf(value: number | string | null | undefined): Tone {
  const n = typeof value === "string" ? Number(value) : value
  return n == null || !Number.isFinite(n) || n === 0 ? "neutral" : n > 0 ? "positive" : "negative"
}
export const toneText: Record<Tone, string> = {
  positive: "text-bullish", negative: "text-bearish", warn: "text-warn", neutral: "", accent: "text-accent",
}

/** A 0-1 progress bar; values outside the range are clamped for display. */
export function Meter({ value, tone = "accent", label }: { value: number | null; tone?: Tone; label: string }) {
  const clamped = value == null || !Number.isFinite(value) ? 0 : Math.min(1, Math.max(0, value))
  const fill = tone === "positive" ? "bg-bullish" : tone === "negative" ? "bg-bearish" : tone === "warn" ? "bg-warn" : "bg-accent"
  return (
    <div role="meter" aria-label={label} aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(clamped * 100)}
      className="h-1.5 overflow-hidden rounded-full bg-raised">
      <div className={`h-full rounded-full ${fill}`} style={{ width: `${clamped * 100}%` }} />
    </div>
  )
}

export function PageHeader({ title, subtitle, children }: { title: ReactNode; subtitle?: ReactNode; children?: ReactNode }) {
  return (
    <div className="flex flex-wrap items-end justify-between gap-3">
      <div className="min-w-0">
        <h1 className="text-lg font-semibold tracking-tight">{title}</h1>
        {subtitle && <div className="mt-0.5 text-xs text-muted">{subtitle}</div>}
      </div>
      {children && <div className="flex flex-wrap items-center gap-2">{children}</div>}
    </div>
  )
}

/** A checklist row: a ticked or open circle, a label and an optional value. */
export function Check({ ok, label, value, tone = "neutral" }: { ok: boolean; label: ReactNode; value?: ReactNode; tone?: Tone }) {
  return (
    <li className="flex items-center justify-between gap-3">
      <span className="flex min-w-0 items-center gap-2">
        <svg viewBox="0 0 16 16" className={`h-3.5 w-3.5 shrink-0 ${ok ? "text-bullish" : "text-faint"}`} fill="none" stroke="currentColor" strokeWidth="1.8" aria-hidden="true">
          <circle cx="8" cy="8" r="6.5" />{ok && <path d="m5 8 2 2 4-4" />}
        </svg>
        <span className="min-w-0 text-muted">{label}</span>
        <span className="sr-only">{ok ? "(met)" : "(not met)"}</span>
      </span>
      {value != null && <span className={`shrink-0 tabular ${toneText[tone]}`}>{value}</span>}
    </li>
  )
}

/** Headline figure with a label, an optional coloured detail line and a meter. */
export function Tile({ label, value, detail, tone = "neutral", meter, hint }: {
  label: string; value: ReactNode; detail?: ReactNode; tone?: Tone; hint?: string
  meter?: { value: number | null; tone?: Tone; label: string }
}) {
  return (
    <div className="min-w-0 rounded-lg border border-border bg-panel px-3 py-2.5" title={hint}>
      <div className="text-[11px] uppercase tracking-wide text-muted">{label}</div>
      <div className={`mt-1 text-xl font-medium tabular [overflow-wrap:anywhere] ${toneText[tone]}`}>{value}</div>
      {detail && <div className="mt-0.5 text-xs text-muted tabular">{detail}</div>}
      {meter && <div className="mt-2"><Meter {...meter} /></div>}
    </div>
  )
}

export function CoverageBadge({ coverage }: { coverage: { label: string; low: boolean } | null }) {
  if (!coverage) return null
  return (
    <span
      className={`rounded-full border px-2 py-0.5 text-[11px] tabular ${coverage.low ? "border-warn text-warn" : "border-border text-muted"}`}
      title={coverage.low ? "Coverage below 90%; analytics may be incomplete" : undefined}
    >
      {coverage.label}{coverage.low ? " · low coverage" : ""}
    </span>
  )
}
