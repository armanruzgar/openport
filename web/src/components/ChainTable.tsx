import { memo, useCallback, useLayoutEffect, useMemo, useRef, useState } from "react"
import type { ChainRow, OptionQuote } from "../api/types"
import type { LiquidityUsed } from "../api/trading-types"
import { count, fixed, isNum, pct, price, vol } from "../lib/format"
import { liquidity, liquidityDetail, quoteIssueText, spreadShare, volumeOiRatio } from "../lib/liquidity"
import { sideFromCell } from "../lib/trading"
import { chainKeyFocus, shortcutAllowed } from "../lib/keyboard"
import { rowWindow } from "../lib/row-window"
import { useMediaQuery } from "../lib/media"
import { Flash } from "./Flash"
import { Segmented } from "./ui"

interface Column {
  key: string
  label: string
  title: string
  render: (o: OptionQuote) => React.ReactNode
  greek?: boolean
  liquidity?: boolean
}

function columns(provider: string): Column[] {
  return [
    { key: "volume", label: "Vol", title: "Session volume (contracts); hover for volume / OI", liquidity: true, render: (o) => <span title={`Volume / OI: ${fixed(volumeOiRatio(o), 2)}`}>{count(o.volume)}</span> },
    { key: "spread", label: "Spr %", title: "Bid-ask spread / quote mid", liquidity: true, render: (o) => pct(spreadShare(o), 1) },
    { key: "liquidity", label: "Liq", title: "Quote liquidity: good, thin or none", liquidity: true, render: (o) => <span className={liquidity(o).cue === "good" ? "text-bullish" : "text-warn"} title={liquidityDetail(o)}>{liquidity(o).cue}</span> },
    { key: "oi", label: "OI", title: "Open interest (contracts)", render: (o) => count(o.oi) },
    { key: "delta", label: "Δ", title: "Delta per unit of underlying", render: (o) => fixed(o.delta, 3) },
    { key: "gamma", label: "Γ", title: "Gamma per $1 of spot", render: (o) => fixed(o.gamma, 5), greek: true },
    { key: "vega", label: "Vega", title: "Per vol point", render: (o) => fixed(o.vega, 3), greek: true },
    { key: "theta", label: "Θ", title: "per calendar day, forward held fixed (as Cboe quotes it)", render: (o) => fixed(o.theta, 3), greek: true },
    {
      key: "iv",
      label: "IV",
      title: `Implied vol from the mid (hover a cell to compare with ${provider})`,
      render: (o) => <span title={[
        isNum(o.eep) && o.eep >= 0.005 ? `IV after removing a $${fixed(o.eep, 2)} early-exercise premium` : null,
        isNum(o.vendor_iv) ? `OpenPort ${vol(o.iv)} · ${provider} ${vol(o.vendor_iv)}` : null,
      ].filter(Boolean).join(" · ") || undefined}>{vol(o.iv)}</span>,
    },
    { key: "bid", label: "Bid", title: "Best bid", render: (o) => <Flash value={o.bid}>{price(o.bid)}</Flash> },
    { key: "ask", label: "Ask", title: "Best offer", render: (o) => <Flash value={o.ask}>{price(o.ask)}</Flash> },
  ]
}

interface Props {
  rows: ChainRow[]; forward: number | null; atmStrike: number | null
  showGreeks: boolean; showLiquidity: boolean; provider: string
  onQuote?: (quote: OptionQuote, row: ChainRow, type: "call" | "put", cell: "bid" | "ask") => void
  selected?: Map<string, "bid" | "ask"> | null
  held?: Map<string, number>; used?: Map<string, LiquidityUsed>
}
const rowHeight = 64
const headerHeight = 44
export function ChainTable({ rows, forward, atmStrike, showGreeks, showLiquidity, provider, onQuote, selected, held, used }: Props) {
  const phone = useMediaQuery("(max-width: 639px)")
  const [side, setSide] = useState<"call" | "put">("call")
  const [focused, setFocused] = useState<{ strike: number; side: "call" | "put" } | null>(null)
  const [scroll, setScroll] = useState({ top: 0, height: 448 })
  const scroller = useRef<HTMLDivElement>(null)
  const pending = useRef<{ strike: number; side: "call" | "put" } | null>(null)
  const action = useRef(onQuote)
  action.current = onQuote
  const choose = useCallback<NonNullable<Props["onQuote"]>>((...args) => action.current?.(...args), [])
  const all = useMemo(() => columns(provider).filter((c) => phone ? ["bid", "ask", "iv"].includes(c.key)
    : (showGreeks || !c.greek) && (showLiquidity || !c.liquidity)), [provider, phone, showGreeks, showLiquidity])
  const calls = useMemo(() => phone && side === "put" ? [] : all, [phone, side, all])
  const puts = useMemo(() => phone && side === "call" ? [] : [...all].reverse(), [phone, side, all])
  const range = rowWindow(rows.length, Math.max(0, scroll.top - headerHeight), scroll.height, rowHeight)
  const count = calls.length + puts.length + 1
  useLayoutEffect(() => {
    const container = scroller.current
    if (!container) return
    const at = rows.findIndex((row) => row.strike === atmStrike)
    container.scrollTop = Math.max(0, at * rowHeight - (container.clientHeight || 448) / 2)
    setScroll({ top: container.scrollTop, height: container.clientHeight || 448 })
  }, [atmStrike]) // Keep scroll stable while quotes update.
  useLayoutEffect(() => {
    const target = pending.current
    if (!target) return
    const button = scroller.current?.querySelector<HTMLButtonElement>(`button[data-strike="${target.strike}"][data-side="${target.side}"][data-cell="ask"]`)
    if (button) { button.focus({ preventScroll: true }); pending.current = null }
  }, [range.start, range.end, focused, side])
  const initial = focused && rows.some((row) => row.strike === focused.strike) ? focused.strike : atmStrike ?? rows[0]?.strike
  return <>
    {phone && <div className="mb-2"><Segmented label="Option side" value={side} options={[{ value: "call", label: "Calls" }, { value: "put", label: "Puts" }]} onChange={(next) => { setSide(next); setFocused(null) }} /></div>}
    <div ref={scroller} data-chain tabIndex={0} aria-label="Chain keyboard navigation" className="relative overflow-auto" style={{ maxHeight: 448, minHeight: 100 }}
      onScroll={(event) => setScroll({ top: event.currentTarget.scrollTop, height: event.currentTarget.clientHeight || 448 })}
      onKeyDown={(event) => {
        if (!shortcutAllowed(event.nativeEvent)) return
        const button = (event.target as HTMLElement).closest<HTMLElement>("[data-strike][data-side]")
        const index = button ? rows.findIndex((row) => row.strike === Number(button.dataset.strike)) : Math.min(rows.length - 1, range.start + 5)
        if (index < 0) return
        const type = (button?.dataset.side ?? side) as "call" | "put"
        const next = chainKeyFocus(event.key, index, type, rows.length)
        if (next) {
          event.preventDefault(); event.stopPropagation()
          const target = { strike: rows[next.index]!.strike, side: next.side }
          if (phone) setSide(next.side)
          pending.current = target; setFocused(target)
          const top = next.index * rowHeight + headerHeight
          const container = scroller.current!
          if (top < container.scrollTop + headerHeight) container.scrollTop = top - headerHeight
          if (top + rowHeight > container.scrollTop + (container.clientHeight || 448)) container.scrollTop = top + rowHeight - (container.clientHeight || 448)
          setScroll({ top: container.scrollTop, height: container.clientHeight || 448 })
          return
        }
        const key = event.key.toLowerCase()
        if (button && onQuote && (key === "b" || key === "s" || key === "enter")) {
          event.preventDefault(); event.stopPropagation()
          const row = rows[index]!, quote = row[type]
          // Enter acts on the focused cell, so Enter on a bid sells, like clicking it.
          const cell = key === "enter" ? button.dataset.cell === "bid" ? "bid" : "ask" : key === "s" ? "bid" : "ask"
          if (quote) choose(quote, row, type, cell)
        }
      }}>
      <table className="w-full border-collapse text-xs" aria-label="Option chain" aria-rowcount={rows.length + 2}>
        <thead className="sticky top-0 z-10 bg-panel" style={{ height: headerHeight }}>
          <tr className="text-[11px] uppercase text-muted">
            {calls.length > 0 && <th colSpan={calls.length} className="text-left font-normal text-gex-positive">Calls</th>}<th />
            {puts.length > 0 && <th colSpan={puts.length} className="text-right font-normal text-gex-negative">Puts</th>}
          </tr>
          <tr className="border-b border-border text-muted">
            {calls.map((c) => <th key={c.key} title={c.title} className="px-2 py-1 text-right font-normal">{c.label}</th>)}
            <th className="px-2 py-1 font-normal">Strike</th>
            {puts.map((c) => <th key={c.key} title={c.title} className="px-2 py-1 text-right font-normal">{c.label}</th>)}
          </tr>
        </thead>
        <tbody>
          {range.before > 0 && <tr aria-hidden="true"><td colSpan={count} style={{ height: range.before, padding: 0 }} /></tr>}
          {rows.slice(range.start, range.end).map((row, index) => <QuoteRow key={row.strike} row={row} calls={calls} puts={puts}
            index={range.start + index + 3} atm={row.strike === atmStrike} callItm={isNum(forward) && row.strike < forward} putItm={isNum(forward) && row.strike > forward}
            callSelected={row.call?.symbol ? selected?.get(row.call.symbol) : undefined} putSelected={row.put?.symbol ? selected?.get(row.put.symbol) : undefined}
            callHeld={row.call?.symbol ? held?.get(row.call.symbol) : undefined} putHeld={row.put?.symbol ? held?.get(row.put.symbol) : undefined}
            callUsed={row.call?.symbol ? used?.get(row.call.symbol) : undefined} putUsed={row.put?.symbol ? used?.get(row.put.symbol) : undefined}
            focused={row.strike === initial ? focused?.side ?? side : null} onQuote={onQuote ? choose : undefined} />)}
          {range.after > 0 && <tr aria-hidden="true"><td colSpan={count} style={{ height: range.after, padding: 0 }} /></tr>}
        </tbody>
      </table>
    </div>
  </>
}
interface RowProps {
  row: ChainRow; calls: Column[]; puts: Column[]; index: number; atm: boolean; callItm: boolean; putItm: boolean
  callSelected?: "bid" | "ask"; putSelected?: "bid" | "ask"; callHeld?: number; putHeld?: number
  callUsed?: LiquidityUsed; putUsed?: LiquidityUsed; focused: "call" | "put" | null; onQuote?: Props["onQuote"]
}
function shallow(a: object | null | undefined, b: object | null | undefined) {
  if (a === b) return true
  if (!a || !b) return false
  const keys = Object.keys(a) as (keyof typeof a)[]
  return keys.length === Object.keys(b).length && keys.every((key) => a[key] === b[key])
}
const QuoteRow = memo(function QuoteRow(p: RowProps) {
  const { row } = p
  const cell = (column: Column, type: "call" | "put") => {
    const quote = row[type], key = column.key
    if (!quote) return "—"
    if (!p.onQuote || (key !== "bid" && key !== "ask")) return column.render(quote)
    const active = p[`${type}Selected`] === key
    const taken = p[`${type}Used`]
    const left = taken ? key === "bid" ? taken.bid_left : taken.ask_left : null
    const size = taken ? key === "bid" ? taken.bid_size : taken.ask_size : null
    const issue = quoteIssueText(quote)
    const note = [issue ? `Not executable: ${issue}` : null, left != null && size != null && left < size ? `${left} of ${size} displayed left for your paper orders` : null].filter(Boolean).join(" · ")
    return <button type="button" data-strike={row.strike} data-side={type} data-cell={key} tabIndex={p.focused === type && key === "ask" ? 0 : -1}
      aria-pressed={active} className={`rounded px-1 underline decoration-dotted underline-offset-4 hover:bg-raised hover:text-accent ${active ? "bg-accent/15 text-accent ring-1 ring-accent/50" : issue ? "text-faint" : key === "bid" ? "text-bearish" : "text-bullish"}`}
      aria-label={`${sideFromCell(key)} ${row.strike} ${type} at ${key} ${price(quote[key])}${quote.tradable ? "" : `: ${quote.untradable_reason ?? "unavailable"}`}${note ? `: ${note}` : ""}`}
      title={[quote.tradable ? `${sideFromCell(key)} paper order` : quote.untradable_reason ?? "Unavailable", note].filter(Boolean).join(" · ")}
      onClick={() => p.onQuote?.(quote, row, type, key)}>{column.render(quote)}
      {left != null && size != null && left < size && <div className="text-[9px] leading-none text-warn">{left} left</div>}</button>
  }
  return <tr aria-rowindex={p.index} style={{ height: rowHeight }} className={`border-b border-border/40 hover:bg-raised/70 ${p.atm ? "outline outline-1 -outline-offset-1 outline-warn/60" : ""}`}>
    {p.calls.map((c) => <td key={c.key} className={`whitespace-nowrap px-2 py-1 text-right tabular ${p.callItm ? "bg-raised/50" : ""}`}>{cell(c, "call")}</td>)}
    <td className="px-2 py-1 text-center tabular">
      <div className={p.atm ? "text-warn" : ""}>{row.strike}</div><div className="text-[10px] text-faint">{vol(row.iv, 1)}</div>
      <div className="flex justify-center gap-1 text-[10px]">{(["call", "put"] as const).map((type) => {
        const quantity = p[`${type}Held`]
        return quantity ? <span key={type} className={quantity > 0 ? "text-bullish" : "text-bearish"}>{type === "call" ? "C" : "P"}{quantity > 0 ? "+" : "−"}{Math.abs(quantity)}</span> : null
      })}</div>
    </td>
    {p.puts.map((c) => <td key={c.key} className={`whitespace-nowrap px-2 py-1 text-right tabular ${p.putItm ? "bg-raised/50" : ""}`}>{cell(c, "put")}</td>)}
  </tr>
}, (a, b) => Object.keys(a).every((key) => {
  if (key === "row") return a.row.strike === b.row.strike && a.row.iv === b.row.iv && shallow(a.row.call, b.row.call) && shallow(a.row.put, b.row.put)
  if (key === "callUsed" || key === "putUsed") return shallow(a[key], b[key])
  return a[key as keyof RowProps] === b[key as keyof RowProps]
}))
