import { useEffect, useRef } from "react"
import type { Expiry } from "../api/types"
import { days, expiryLabel } from "../lib/format"
import { groupKeyIndex } from "../lib/keyboard"

/// Horizontal strip of expiries with days to expiry. Shows the settlement only where
/// two expiries share a date (SPX monthly AM vs SPXW PM).
export function ExpiryPicker({ expiries, value, onChange }: { expiries: Expiry[]; value: string | null; onChange: (id: string) => void }) {
  const strip = useRef<HTMLDivElement>(null)
  const selected = useRef<HTMLButtonElement>(null)
  useEffect(() => {
    // Scroll only the strip sideways; scrollIntoView would also scroll the page.
    const container = strip.current
    const button = selected.current
    if (!container || !button) return
    // The strip is positioned, so the button's offset is already measured from it.
    const left = button.offsetLeft
    if (left < container.scrollLeft || left + button.offsetWidth > container.scrollLeft + container.clientWidth) {
      container.scrollTo({ left: left - container.clientWidth / 3, behavior: "smooth" })
    }
  }, [value])

  const dates = new Map<string, number>()
  for (const e of expiries) dates.set(e.expiry, (dates.get(e.expiry) ?? 0) + 1)

  return (
    <div ref={strip} className="relative flex gap-1 overflow-x-auto pb-1" role="tablist" aria-label="Expiry">
      {expiries.map((e, index) => {
        const active = e.id === value
        return (
          <button
            key={e.id}
            ref={active ? selected : undefined}
            role="tab"
            aria-selected={active}
            tabIndex={active ? 0 : -1}
            onClick={() => onChange(e.id)}
            onKeyDown={(event) => {
              if (event.altKey || event.ctrlKey || event.metaKey) return
              const next = groupKeyIndex(event.key, index, expiries.length)
              if (next == null) return
              const expiry = expiries[next]
              if (!expiry) return
              event.preventDefault()
              event.stopPropagation()
              onChange(expiry.id)
              strip.current?.querySelectorAll<HTMLButtonElement>('[role="tab"]').item(next)?.focus()
            }}
            className={`shrink-0 rounded-md border px-2 py-1 text-left transition-colors ${
              active ? "border-accent/60 bg-raised" : "border-border hover:border-muted"
            }`}
          >
            <div className="text-xs">{expiryLabel(e.id, (dates.get(e.expiry) ?? 0) > 1)}</div>
            <div className="text-[10px] text-muted tabular">{days(e.days)}</div>
          </button>
        )
      })}
    </div>
  )
}
