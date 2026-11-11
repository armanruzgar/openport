import { useQuery } from "@tanstack/react-query"
import { useEffect, useState } from "react"
import { api } from "../api/client"
import { marketTime, useLive } from "../api/live"
import type { UnderlyingSnapshot, UnderlyingSubscription } from "../api/types"
import { errorText } from "../lib/errors"
import { barDay, expectedMove } from "../lib/candles"
import { defaultExpiry } from "../lib/expiry"
import { fixed, isNum, pct, price, vol } from "../lib/format"
import { timestampET } from "../lib/freshness"
import { matchingPayload } from "../lib/payload"
import { Dialog } from "./Dialog"

export function searchSymbols(symbols: readonly string[], query: string) {
  const needle = query.trim().toUpperCase()
  return symbols.filter((symbol) => symbol.toUpperCase().includes(needle))
}
/** A symbol the server takes for a runtime subscription, in capitals; null for anything else. */
export function subscribableSymbol(text: string): string | null {
  const symbol = text.trim().toUpperCase()
  return /^[A-Z0-9][A-Z0-9.]{0,15}$/.test(symbol) ? symbol : null
}
function WatchSymbol({ underlying, onSelect }: { underlying: UnderlyingSnapshot; onSelect: (symbol: string) => void }) {
  const live = useLive()
  const { symbol, spot, as_of } = underlying
  const version = live.version(symbol)
  // Each version bump keeps the last figures on the card while the next ones load.
  const summary = useQuery({ queryKey: ["summary", symbol, version], queryFn: ({ signal }) => api.summary(symbol, signal),
    placeholderData: (previous) => matchingPayload(previous, symbol) }).data
  const volatility = useQuery({ queryKey: ["volatility", symbol, version], queryFn: ({ signal }) => api.volatility(symbol, signal),
    placeholderData: (previous) => matchingPayload(previous, symbol) }).data
  const day = as_of && Number.isFinite(Date.parse(as_of)) ? barDay(Date.parse(as_of) / 1000) : null
  // The prior day's close does not move during the day: ask once per New York trading day,
  // and again each minute only until that bar has arrived (vendor history comes later).
  const candles = useQuery({ queryKey: ["watch-close", symbol, day], queryFn: ({ signal }) => api.candles(symbol, "1d", 10, signal),
    enabled: day != null, placeholderData: (previous) => previous?.symbol === symbol ? previous : undefined,
    refetchInterval: (query) => day != null && query.state.data?.bars.some((bar) => barDay(bar.t) < day) ? false : 60_000 }).data
  const data = matchingPayload(summary, symbol)
  const expiry = data?.expiries.find((e) => e.id === defaultExpiry(data.expiries, marketTime(live, symbol, data.as_of)))
  const previous = day && candles?.symbol === symbol ? candles.bars.filter((bar) => barDay(bar.t) < day).at(-1)?.c : null
  const change = isNum(spot) && isNum(previous) && previous > 0 ? (spot / previous - 1) * 100 : null
  const rank = volatility?.symbol === symbol ? volatility.iv_rank : null
  return <li className="min-w-0 rounded-md border border-border p-3">
    <button type="button" className="flex w-full items-center justify-between gap-2 text-left" onClick={() => onSelect(symbol)}>
      <span className="font-medium text-accent">{symbol}</span><span className="tabular">{data?.spot_source === "parity" ? "≈" : ""}{price(spot)}</span>
    </button>
    <dl className="mt-2 grid grid-cols-2 gap-2 text-xs">
      <div><dt className="text-muted">Change vs prior daily bar</dt><dd className="tabular">{isNum(change) ? `${change >= 0 ? "+" : ""}${fixed(change, 2)}%` : "—"}</dd></div>
      <div><dt className="text-muted">ATM IV</dt><dd>{vol(expiry?.atm_iv)}</dd></div>
      <div><dt className="text-muted">IV rank</dt><dd title={volatility?.proxy?.used ? `Includes ${volatility.proxy.name ?? "index"} proxy history` : "Own available history"}>{pct(rank, 1)}{volatility?.proxy?.used ? " · proxy" : ""}</dd></div>
      <div><dt className="text-muted">Derived ±1σ move</dt><dd>{price(expectedMove(spot, expiry?.atm_iv, expiry?.days))}</dd></div>
    </dl>
    <p className="mt-2 break-words text-[10px] text-faint">{as_of ? `Market data: ${timestampET(as_of)}` : "Market time unavailable"}{expiry ? ` · ${expiry.id}` : ""}</p>
  </li>
}
export function Watchlist({ onSymbol }: { onSymbol: (symbol: string) => void }) {
  const { underlyings, status, source, replay } = useLive()
  const [open, setOpen] = useState(false)
  return <section className="mb-3 min-w-0 rounded-lg border border-border bg-panel p-3">
    <button className="text-sm" aria-expanded={open} onClick={() => setOpen(!open)}>Watchlist {open ? "▾" : "▸"} <span className="text-xs text-muted">{underlyings.length} subscribed symbols</span></button>
    {open && <>
      <p className="my-2 text-xs text-muted">{status?.provider.simulated || (source === "replay" && replay?.demo) ? "Simulated data · " : ""}Feed prices; missing metrics stay blank. Expected move uses the nearest active expiry. Subscribe to another underlying from symbol search (Ctrl/⌘ K).</p>
      <ul className="grid gap-2 sm:grid-cols-2 xl:grid-cols-4">{underlyings.map((u) => <WatchSymbol key={u.symbol} underlying={u} onSelect={onSymbol} />)}</ul>
    </>}
  </section>
}
export function SymbolSearch({ onSymbol, onClose }: { onSymbol: (symbol: string) => void; onClose: () => void }) {
  const { underlyings, source, trading } = useLive()
  const [query, setQuery] = useState("")
  const [index, setIndex] = useState(0)
  const matches = searchSymbols(underlyings.map((u) => u.symbol), query)
  // The list can shrink while open (a symbol unsubscribed): keep the highlight on a row.
  const active = Math.min(index, matches.length - 1)
  // aria-activedescendant scrolls nothing: bring the highlighted row into view.
  useEffect(() => {
    if (active >= 0) document.getElementById(`symbol-result-${active}`)?.scrollIntoView?.({ block: "nearest" })
  }, [active])
  // Whether the live server can add underlyings while it runs; a replay plays its own.
  const [subscription, setSubscription] = useState<UnderlyingSubscription | null>(null)
  const [change, setChange] = useState<{ pending: boolean; error: string | null }>({ pending: false, error: null })
  useEffect(() => {
    if (source !== "live") return
    const request = new AbortController()
    api.underlyingSubscription(request.signal).then(setSubscription, () => {})
    return () => request.abort()
  }, [source])
  const mode = source === "live" ? trading?.write ?? "disabled" : "disabled"
  const typed = subscribableSymbol(query)
  const offer = typed && subscription?.live && !subscription.underlyings.includes(typed) &&
    !underlyings.some((u) => u.symbol === typed) ? typed : null
  const choose = (symbol: string) => { onSymbol(symbol); onClose() }
  async function update(run: () => Promise<UnderlyingSubscription>, done?: () => void) {
    setChange({ pending: true, error: null })
    try {
      setSubscription(await run())
      setChange({ pending: false, error: null })
      done?.()
    } catch (error) {
      setChange({ pending: false, error: errorText(error) })
    }
  }
  // Its views wait for the first snapshot, which arrives with the feed's next poll.
  const subscribe = (symbol: string) => update(() => api.subscribeUnderlying(symbol, mode), () => choose(symbol))
  return <Dialog title="Jump to symbol" onClose={onClose}>
    <input autoFocus className="trade-input" role="combobox" aria-label="Search subscribed symbols" aria-controls="symbol-results" aria-expanded="true"
      aria-activedescendant={active >= 0 ? `symbol-result-${active}` : undefined} value={query}
      onChange={(e) => { setQuery(e.target.value); setIndex(0) }} onKeyDown={(event) => {
        if (event.key === "ArrowDown" || event.key === "ArrowUp") { event.preventDefault(); setIndex(Math.max(0, Math.min(matches.length - 1, active + (event.key === "ArrowDown" ? 1 : -1)))) }
        const chosen = matches[active]
        if (event.key === "Enter" && chosen) { event.preventDefault(); choose(chosen) }
        else if (event.key === "Enter" && offer && !change.pending && mode !== "disabled") { event.preventDefault(); void subscribe(offer) }
      }} />
    <ul id="symbol-results" role="listbox" aria-label="Subscribed symbols" className="max-h-80 overflow-auto">{matches.map((symbol, i) =>
      <li id={`symbol-result-${i}`} role="option" aria-selected={i === active} key={symbol} className={i === active ? "bg-raised" : ""}>
        <button className="w-full p-3 text-left" onClick={() => choose(symbol)}>{symbol} <span className="float-right tabular">{price(underlyings.find((u) => u.symbol === symbol)?.spot)}</span></button>
      </li>)}</ul>
    {!matches.length && <p className="text-sm text-muted">No subscribed symbol matches.</p>}
    {offer && <div className="space-y-1">
      <button type="button" className="trade-button" disabled={change.pending || mode === "disabled"} onClick={() => void subscribe(offer)}>Subscribe to {offer}</button>
      <p className="text-xs text-muted">The server polls it from the feed's next cycle until openportd restarts; recordings keep their own symbols.</p>
    </div>}
    {!offer && typed && !matches.length && subscription && !subscription.live && subscription.reason &&
      <p className="text-xs text-muted">{subscription.reason}.</p>}
    {subscription && subscription.added.length > 0 && <section className="space-y-1" aria-label="Added while running">
      <h3 className="text-xs font-medium uppercase tracking-wide text-muted">Added while running</h3>
      <ul className="flex flex-wrap gap-2">{subscription.added.map((symbol) => <li key={symbol}>
        <button type="button" className="trade-button" aria-label={`Unsubscribe from ${symbol}`} disabled={change.pending || mode === "disabled"}
          onClick={() => void update(() => api.unsubscribeUnderlying(symbol, mode))}>{symbol} ×</button>
      </li>)}</ul>
    </section>}
    {change.error && <p role="alert" className="text-xs text-danger">{change.error}</p>}
  </Dialog>
}
