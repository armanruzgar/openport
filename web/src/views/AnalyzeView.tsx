import { useQueries, useQuery } from "@tanstack/react-query"
import { useEffect, useState } from "react"
import { api } from "../api/client"
import { marketTime, useLive } from "../api/live"
import { sameAccountPlaceholder, usePortfolio } from "../api/trading"
import type { RiskProfile, RiskProfileQuery } from "../api/trading-types"
import type { Chain, StrategyCandidate } from "../api/types"
import { LineChart, type Marker, type Reference, type Series } from "../charts/LineChart"
import { AnalyzeTicket, quotedLegs, useExpiryChains } from "../components/AnalyzeTicket"
import { StrategyOptimizer } from "../components/StrategyOptimizer"
import { TradingError } from "../components/TradingControls"
import { Empty, Panel, Segmented } from "../components/ui"
import { addAnalyzeCandidate, candidateLegs, daysUntil, entryCost, maxAnalyzeCandidates, moveLeg, nearestRow, profileLegs,
  sliceRows, stepStrike, updateAnalyze, useAnalyze, valueAt, zeroCrossings, type AnalyzeCandidate } from "../lib/analyze"
import { defaultExpiry } from "../lib/expiry"
import { expiryLabel, fixed, money, price } from "../lib/format"
import { matchingPayload } from "../lib/payload"
import { parseNumbers } from "../lib/probability"
import { MAX_LEGS, MAX_RATIO, strategyLabel, type StrategyLeg } from "../lib/strategy"

const colours = ["var(--chart-1)", "var(--chart-2)", "var(--chart-3)", "var(--chart-4)"]
const signed = (value: number | null | undefined) => value == null ? "—" : `${value > 0 ? "+" : value < 0 ? "−" : ""}$${Math.abs(value).toLocaleString("en-US", { maximumFractionDigits: 0 })}`
/** A date the curves are drawn on: today, a custom number of days out, or the candidate's first expiry. */
interface DateLine { key: "today" | "custom" | "expiry"; days: number; label: string }

/** The value after `ms` without a change, so dragging and sliding do not ask for every step. */
function useSettled<T>(value: T, ms = 250): T {
  const text = JSON.stringify(value)
  const [settled, setSettled] = useState(text)
  useEffect(() => { const timer = setTimeout(() => setSettled(text), ms); return () => clearTimeout(timer) }, [text, ms])
  return JSON.parse(settled) as T
}

/** The lines a candidate is drawn on; its expiry line is at least a minute past its first leg's expiry, so those legs are at intrinsic. */
export function dateLines(candidate: AnalyzeCandidate, chains: ReadonlyMap<string, Chain>, now: number, custom: number | null): DateLine[] {
  const times = candidate.legs.map((leg) => chains.get(leg.expiry)?.expiry.expiry_time).filter((t): t is string => t != null)
  const lines: DateLine[] = [{ key: "today", days: 0, label: "Today" }]
  if (custom != null) lines.push({ key: "custom", days: custom, label: `T+${custom}` })
  if (times.length && Number.isFinite(now)) {
    // Rounded up to the hundredth of a day, so the line stays put as the clock ticks.
    const days = Math.min(366, Math.ceil((Math.min(...times.map((t) => daysUntil(t, now))) + 1 / 1440) * 100) / 100)
    if (!lines.some((line) => line.days === days)) lines.push({ key: "expiry", days, label: "Expiry" })
  }
  return lines.sort((a, b) => a.days - b.days)
}
/** The candidate's own P&L on the profile's prices: its shares' move added, its entry cost beyond the mids taken off. */
export function candidateCurve(profile: RiskProfile, index: number, candidate: AnalyzeCandidate, basis: "mid" | "natural"): (number | null)[] {
  const cost = entryCost(candidate.legs, candidate.units, basis)
  const spot = profile.spot
  return profile.prices.map((p, i) => {
    const value = profile.curves[index]?.pnl[i]
    if (value == null || p == null || cost == null || (candidate.shares && spot == null)) return null
    return value + (candidate.shares ? candidate.shares * candidate.units * (p - spot!) : 0) - cost
  })
}

function Slices({ spot, candidates, profiles, lines, basis }: {
  spot: number; candidates: AnalyzeCandidate[]; profiles: (RiskProfile | undefined)[]; lines: DateLine[][]; basis: "mid" | "natural"
}) {
  const [percentText, setPercentText] = useState("-10, -5, -2, -1, 0, 1, 2, 5, 10")
  const [priceText, setPriceText] = useState("")
  const [kinds, setKinds] = useState({ percent: true, price: true, sigma: true, breakeven: true })
  const signedList = (text: string) => {
    const values = text.split(",").map((t) => t.trim()).filter(Boolean).map(Number)
    return values.every((v) => Number.isFinite(v) && v > -100 && v <= 1000) && values.length <= 20 ? [...new Set(values)] : null
  }
  const percents = signedList(percentText)
  const prices = parseNumbers(priceText, 20, Number.MIN_VALUE, 1e9)
  const first = profiles.findIndex(Boolean)
  const firstProfile = profiles[first]
  const sigma = firstProfile?.curves.at(-1)?.horizon.sigma ?? null
  const breakevens = candidates.flatMap((c, i) => {
    const profile = profiles[i]
    if (!profile) return []
    const expiry = lines[i]!.length - 1
    return zeroCrossings(profile.prices.map(Number), candidateCurve(profile, expiry, c, basis)).map((b) => ({ label: `BE ${c.name}`, price: b }))
  })
  const rows = sliceRows(spot, { percents: kinds.percent ? percents ?? [] : [], prices: kinds.price ? prices ?? [] : [],
    sigma: kinds.sigma ? sigma : null, breakevens: kinds.breakeven ? breakevens : [] })
  const columns = candidates.flatMap((c, i) => profiles[i] ? lines[i]!.map((line, j) => ({ c, i, j, line })) : [])
  return <div className="space-y-2">
    <div className="flex flex-wrap items-end gap-3 text-xs">
      {(["percent", "price", "sigma", "breakeven"] as const).map((kind) => <label key={kind} className="flex items-center gap-1 text-muted">
        <input type="checkbox" checked={kinds[kind]} onChange={(e) => setKinds((k) => ({ ...k, [kind]: e.target.checked }))} />
        {kind === "percent" ? "% moves" : kind === "price" ? "$ prices" : kind === "sigma" ? "±σ" : "Breakevens"}</label>)}
      <label className="flex flex-col gap-1 text-muted">Moves (%)
        <input aria-label="Slice moves in percent" className={`trade-input w-56 ${percents ? "" : "border-danger"}`} value={percentText} onChange={(e) => setPercentText(e.target.value)} /></label>
      <label className="flex flex-col gap-1 text-muted">Prices
        <input aria-label="Slice prices" className={`trade-input w-40 ${prices ? "" : "border-danger"}`} placeholder="e.g. 4900, 5100" value={priceText} onChange={(e) => setPriceText(e.target.value)} /></label>
    </div>
    <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Price slices">
      <table className="w-full text-right text-xs tabular whitespace-nowrap">
        <thead className="text-muted"><tr><th className="py-1 pr-2 text-left font-normal">Slice</th><th className="pr-2 font-normal">Price</th>
          {columns.map(({ c, line }) => <th key={`${c.id}-${line.key}`} className="pr-2 font-normal">{c.name} · {line.label}</th>)}</tr></thead>
        <tbody>{rows.map((row) => <tr key={row.key} className="border-t border-border/40">
          <td className="py-1 pr-2 text-left">{row.label}</td><td className="pr-2">{price(row.price)}</td>
          {columns.map(({ c, i, j, line }) => {
            const profile = profiles[i]!
            const value = valueAt(profile.prices.map(Number), candidateCurve(profile, j, c, basis), row.price)
            return <td key={`${c.id}-${line.key}`} className={`pr-2 ${value != null && value < 0 ? "text-bearish" : ""}`}>{signed(value)}</td>
          })}
        </tr>)}</tbody>
      </table>
    </div>
    {!rows.length && <p className="text-xs text-muted">Pick the slices to tabulate.</p>}
    <p className="text-[11px] text-muted">Values between the curve's points are linear; prices outside the chart's range show —. σ is the
      at-the-money implied standard deviation of the log price to the last date.</p>
  </div>
}

function LegRow({ leg, chain, expiries, onChange, onRemove }: {
  leg: StrategyLeg; chain: Chain | undefined; expiries: { id: string }[]; onChange: (leg: StrategyLeg) => void; onRemove: () => void
}) {
  const rows = (chain?.strikes ?? []).filter((row) => row[leg.type]?.tradable && row[leg.type]?.symbol)
  const step = (direction: 1 | -1) => { const row = chain && stepStrike(chain.strikes, leg, direction); if (row) onChange(moveLeg(leg, row)) }
  const retype = (type: StrategyLeg["type"]) => {
    const row = chain && nearestRow(chain.strikes, type, leg.strike)
    if (row) onChange(moveLeg({ ...leg, type }, row))
  }
  const label = `${leg.strike} ${leg.type}`
  return <tr className="border-t border-border/40">
    <td className="py-1 pr-2"><select aria-label={`Side of ${label}`} className="trade-input !w-auto !py-0.5" value={leg.side}
      onChange={(e) => onChange({ ...leg, side: e.target.value as StrategyLeg["side"] })}><option value="buy">Buy</option><option value="sell">Sell</option></select></td>
    <td className="pr-2"><input aria-label={`Ratio of ${label}`} type="number" min={1} max={MAX_RATIO} className="trade-input w-14 !py-0.5" value={leg.ratio}
      onChange={(e) => { const r = Number(e.target.value); if (Number.isInteger(r) && r >= 1 && r <= MAX_RATIO) onChange({ ...leg, ratio: r }) }} /></td>
    <td className="pr-2"><select aria-label={`Type of ${label}`} className="trade-input !w-auto !py-0.5" value={leg.type}
      onChange={(e) => retype(e.target.value as StrategyLeg["type"])}><option value="call">Call</option><option value="put">Put</option></select></td>
    <td className="pr-2"><select aria-label={`Expiry of ${label}`} className="trade-input !w-auto !py-0.5" value={leg.expiry}
      onChange={(e) => onChange({ ...leg, expiry: e.target.value, symbol: "", quote: null })}>
      {expiries.map((e) => <option key={e.id} value={e.id}>{expiryLabel(e.id, true)}</option>)}</select></td>
    <td className="pr-2"><div className="flex items-center gap-1">
      <button type="button" className="trade-button !px-1.5 !py-0.5" aria-label={`Lower strike of ${label}`} disabled={!chain} onClick={() => step(-1)}>−</button>
      <select aria-label={`Strike of ${label}`} className="trade-input !w-auto !py-0.5" value={leg.strike} disabled={!chain}
        onChange={(e) => { const row = rows.find((r) => r.strike === Number(e.target.value)); if (row) onChange(moveLeg(leg, row)) }}>
        {!rows.some((r) => r.strike === leg.strike) && <option value={leg.strike}>{leg.strike}</option>}
        {rows.map((r) => <option key={r.strike} value={r.strike}>{r.strike}</option>)}</select>
      <button type="button" className="trade-button !px-1.5 !py-0.5" aria-label={`Raise strike of ${label}`} disabled={!chain} onClick={() => step(1)}>+</button>
    </div></td>
    <td className="pr-2 text-right tabular">{leg.symbol ? `${price(leg.quote?.bid)} × ${price(leg.quote?.ask)}` : "Loading…"}</td>
    <td className="text-right"><button type="button" className="text-muted hover:text-foreground" aria-label={`Remove ${label}`} onClick={onRemove}>×</button></td>
  </tr>
}

/**
 * The Analyze tab: candidate strategies' P&L by price on today's, a custom and
 * their first expiry's date, with or without the held book, under an IV shift;
 * price slices as a table; strikes dragged on the chart or stepped beside each
 * leg; and the strategy optimizer. Curves come from GET /api/risk/profile with
 * the candidate's legs, the same model the risk profile draws the held book with.
 */
export function AnalyzeView({ symbol }: { symbol: string }) {
  const live = useLive()
  const state = useAnalyze(symbol)
  const summary = useQuery({
    queryKey: ["summary", symbol, live.version(symbol)],
    queryFn: ({ signal }) => api.summary(symbol, signal),
    placeholderData: (previous) => matchingPayload(previous, symbol),
  })
  const data = matchingPayload(summary.data, symbol)
  const expiries = data?.expiries ?? []
  const now = marketTime(live, symbol, data?.as_of)
  const front = defaultExpiry(expiries, now)
  const [held, setHeld] = useState(true)
  const [iv, setIv] = useState(0)
  const [customText, setCustomText] = useState("")
  const [range, setRange] = useState(10)
  const [basis, setBasis] = useState<"mid" | "natural">("natural")
  const [ticket, setTicket] = useState<{ legs: StrategyLeg[]; units: number; shares: number } | null>(null)
  const { chains } = useExpiryChains(symbol, [...state.candidates.flatMap((c) => c.legs.map((leg) => leg.expiry)), ...(front ? [front] : [])])
  const candidates = state.candidates.map((c) => ({ ...c, legs: quotedLegs(c.legs, chains) }))
  // A leg moved to another expiry takes that chain's nearest strike once it loads.
  useEffect(() => {
    const pending = state.candidates.some((c) => c.legs.some((leg) => !leg.symbol && chains.has(leg.expiry)))
    if (!pending) return
    updateAnalyze(symbol, (s) => ({ ...s, candidates: s.candidates.map((c) => ({ ...c, legs: c.legs.map((leg) => {
      const chain = chains.get(leg.expiry)
      const row = !leg.symbol && chain ? nearestRow(chain.strikes, leg.type, leg.strike) : null
      return row ? moveLeg(leg, row) : leg
    }) })) }))
  }, [state.candidates, chains, symbol])
  const portfolio = usePortfolio().data
  const holds = !!portfolio && (portfolio.positions.some((p) => p.underlying === symbol && !p.awaiting_settlement) || (portfolio.stocks ?? []).some((s) => s.symbol === symbol))
  const withHeld = held && holds
  const customDays = /^\d+(\.\d+)?$/.test(customText.trim()) && Number(customText) > 0 && Number(customText) <= 366 ? Number(customText) : null
  const visible = candidates.filter((c) => c.visible && c.legs.length && c.legs.every((leg) => leg.symbol))
  const lines = visible.map((c) => dateLines(c, chains, now, customDays))
  const queries: RiskProfileQuery[] = visible.map((c, i) => ({ underlying: symbol, days: lines[i]!.map((line) => line.days), iv, range, steps: 81,
    legs: profileLegs(c.legs, c.units), held: withHeld }))
  const settled = useSettled({ entries: visible.map((c, i) => ({ id: c.id, query: queries[i]! })), heldDays: lines[0]?.map((line) => line.days) ?? [0] })
  const enabled = !!live.trading?.enabled
  const profiles = useQueries({ queries: settled.entries.map(({ query }) => ({
    queryKey: ["trading", live.accountScope, "analyze", live.trading?.account_version, query],
    queryFn: ({ signal }: { signal: AbortSignal }) => api.riskProfile(query, signal),
    enabled, retry: false,
    placeholderData: (previous: RiskProfile | undefined, prior: { queryKey: readonly unknown[] } | undefined) => sameAccountPlaceholder(previous, prior?.queryKey, live.accountScope),
  })) })
  const heldQuery: RiskProfileQuery = { underlying: symbol, days: settled.heldDays, iv: settled.entries[0]?.query.iv ?? iv, range, steps: 81 }
  const heldProfile = useQuery({
    queryKey: ["trading", live.accountScope, "analyze-held", live.trading?.account_version, heldQuery],
    queryFn: ({ signal }) => api.riskProfile(heldQuery, signal),
    enabled: enabled && withHeld, retry: false,
    placeholderData: (previous, prior) => sameAccountPlaceholder(previous, prior?.queryKey, live.accountScope),
  })
  // A profile answers the query it was asked; until the settled query catches up, the curves wait.
  const results = visible.map((c, i) => {
    const at = settled.entries.findIndex((entry) => entry.id === c.id)
    const profile = at >= 0 ? profiles[at]?.data : undefined
    return profile && profile.curves.length === lines[i]!.length ? profile : undefined
  })
  const selected = candidates.find((c) => c.id === state.selected) ?? candidates[0]
  const update = (id: string, change: (c: AnalyzeCandidate) => AnalyzeCandidate) =>
    updateAnalyze(symbol, (s) => ({ ...s, candidates: s.candidates.map((c) => c.id === id ? change(c) : c) }))
  const setLeg = (id: string, index: number, leg: StrategyLeg) => update(id, (c) => ({ ...c, legs: c.legs.map((l, i) => i === index ? leg : l) }))
  const atmLeg = (expiry: string, type: StrategyLeg["type"]): StrategyLeg | null => {
    const chain = chains.get(expiry)
    const row = chain && nearestRow(chain.strikes, type, chain.expiry.forward ?? chain.spot ?? 0)
    return row ? moveLeg({ symbol: "", underlying: symbol, side: "buy", ratio: 1, type, strike: row.strike, expiry, quote: null }, row) : null
  }
  const spot = results.find(Boolean)?.spot ?? data?.spot ?? null

  const chart = (() => {
    const series: Series[] = []
    const markers: Marker[] = []
    const references: Reference[] = []
    visible.forEach((c, i) => {
      const profile = results[i]
      if (!profile) return
      const xs = profile.prices.map(Number)
      lines[i]!.forEach((line, j) => series.push({ id: `${c.id}-${line.key}`, label: `${c.name} · ${line.label}`, color: colours[i % colours.length]!,
        dashed: line.key !== "expiry", points: xs.map((x, k) => ({ x, y: candidateCurve(profile, j, c, basis)[k] ?? null })) }))
    })
    const heldData = withHeld ? heldProfile.data : undefined
    if (heldData && heldData.complete) heldData.curves.forEach((curve, j) => series.push({ id: `held-${j}`, label: `Held only · T+${fixed(curve.days, 1)}`,
      color: "var(--muted)", dashed: true, points: heldData.prices.map((x, k) => ({ x: Number(x), y: curve.pnl[k] ?? null })) }))
    const room = withHeld ? results.find(Boolean)?.room : null
    if (room != null && Number.isFinite(Number(room))) references.push({ y: -Number(room), label: "Plan floor", color: "var(--bearish)" })
    if (spot != null) markers.push({ x: spot, label: `${symbol} now`, color: "var(--muted)" })
    const index = visible.findIndex((c) => c.id === selected?.id)
    const profile = results[index]
    if (selected && profile) {
      const [low, high] = [Number(profile.prices[0]), Number(profile.prices.at(-1))]
      selected.legs.forEach((leg, legIndex) => {
        const chain = chains.get(leg.expiry)
        if (!(leg.strike >= low && leg.strike <= high)) return
        markers.push({ x: leg.strike, color: colours[index % colours.length]!,
          label: `${leg.side === "buy" ? "+" : "−"}${leg.ratio > 1 ? leg.ratio : ""}${leg.strike}${leg.type === "call" ? "C" : "P"}`,
          onDrag: (x) => { const row = chain && nearestRow(chain.strikes, leg.type, x); if (row && row.strike !== leg.strike) setLeg(selected.id, legIndex, moveLeg(leg, row)) },
          onStep: (direction) => { const row = chain && stepStrike(chain.strikes, leg, direction); if (row) setLeg(selected.id, legIndex, moveLeg(leg, row)) } })
      })
      const cone = profile.curves.at(-1)?.horizon.one_sd
      if (cone) for (const x of [cone.low, cone.high]) if (x >= low && x <= high) markers.push({ x, label: "1σ", color: "var(--accent)" })
    }
    return { series, markers, references }
  })()

  if (!live.trading) return <Empty>Analyze draws strategies through the paper account's risk profile; paper trading is unavailable on this server.</Empty>
  if (summary.isError) return <Empty>{String(summary.error)}</Empty>
  if (!data) return <Empty>Loading {symbol}…</Empty>
  const errors = profiles.map((p) => p.error).filter(Boolean)
  return <div className="flex flex-col gap-3">
    <Panel title={`Analyze ${symbol}`} actions={<>
      <label className="flex items-center gap-2 text-xs text-muted"><input type="checkbox" checked={held} disabled={!holds} onChange={(e) => setHeld(e.target.checked)} />
        Include held {symbol} positions</label>
      <label className="flex items-center gap-2 text-xs text-muted">IV shift <span className="w-12 text-right tabular text-foreground">{iv > 0 ? "+" : ""}{iv} vp</span>
        <input aria-label="IV shift in vol points" type="range" min={-20} max={20} step={1} value={iv} onChange={(e) => setIv(Number(e.target.value))} /></label>
      <label className="flex items-center gap-1 text-xs text-muted">Custom date T+
        <input aria-label="Custom date in days" className={`trade-input w-16 !py-0.5 ${customText && customDays == null ? "border-danger" : ""}`} placeholder="days"
          value={customText} onChange={(e) => setCustomText(e.target.value)} /></label>
      <Segmented label="Price range" value={range} onChange={setRange} options={[5, 10, 20].map((r) => ({ value: r, label: `±${r}%` }))} />
      <Segmented label="Entry price" value={basis} onChange={setBasis} options={[{ value: "natural", label: "Natural" }, { value: "mid", label: "Mid" }]} />
    </>}>
      {errors.map((error, i) => <TradingError key={i} error={error} />)}
      {visible.length ? <LineChart series={chart.series} markers={chart.markers} references={chart.references} height={320} marginLeft={70}
        xLabel={`${symbol} price`} formatX={(x) => fixed(x, x >= 1000 ? 0 : 2)} formatY={money} />
        : <p className="text-sm text-muted">Add a candidate below, or analyze one from the optimizer, to draw its P&amp;L.</p>}
      <p className="mt-2 text-[11px] text-muted">Solid: each candidate at its first expiry; dashed: today and the custom date{withHeld ? "; candidates include the held book, grey is the held book alone" : ""}.
        {" "}Drag a strike's handle on the chart (or focus it and use the arrow keys) to move the selected candidate's leg to the nearest listed strike.
        {" "}P&amp;L is from the contracts' model values now{basis === "natural" ? ", less the spread paid at the far sides" : ", entered at mids"}, before fees.
        {" "}{results.find(Boolean)?.model ?? ""}</p>
    </Panel>

    <Panel title={`Candidates · ${candidates.length} of ${maxAnalyzeCandidates}`} actions={<>
      <button type="button" className="trade-button" disabled={candidates.length >= maxAnalyzeCandidates || !front || !chains.get(front)}
        onClick={() => { const leg = front && atmLeg(front, "call"); if (leg) addAnalyzeCandidate(symbol, { name: `Candidate ${candidates.length + 1}`, legs: [leg], units: 1, shares: 0 }) }}>
        New candidate</button>
    </>}>
      {!candidates.length && <p className="text-sm text-muted">A candidate starts as an at-the-money call; change its legs, or analyze an optimizer result.</p>}
      <div className="space-y-3">{candidates.map((c, i) => <article key={c.id} className="rounded-md border border-border p-2 text-xs">
        <div className="flex flex-wrap items-center gap-2">
          <span aria-hidden className="inline-block h-2 w-2 rounded-full" style={{ background: colours[i % colours.length] }} />
          <label className="flex items-center gap-1"><input type="checkbox" checked={c.visible} aria-label={`Show ${c.name}`} onChange={(e) => update(c.id, (x) => ({ ...x, visible: e.target.checked }))} /> Show</label>
          <label className="flex items-center gap-1"><input type="radio" name={`analyze-${symbol}`} checked={selected?.id === c.id} aria-label={`Drag ${c.name}'s strikes`}
            onChange={() => updateAnalyze(symbol, (s) => ({ ...s, selected: c.id }))} /> Drag strikes</label>
          <input className="trade-input w-48 !py-0.5" aria-label="Candidate name" value={c.name} maxLength={64} onChange={(e) => update(c.id, (x) => ({ ...x, name: e.target.value }))} />
          <label className="flex items-center gap-1 text-muted">Units
            <input type="number" min={1} max={1000} className="trade-input w-16 !py-0.5" aria-label={`Units of ${c.name}`} value={c.units}
              onChange={(e) => { const u = Number(e.target.value); if (Number.isInteger(u) && u >= 1 && u <= 1000) update(c.id, (x) => ({ ...x, units: u })) }} /></label>
          <span className="text-muted">{c.legs.length ? strategyLabel(c.legs) : "No legs"}{c.shares ? ` · +${c.shares * c.units} shares` : ""}</span>
          <span className="ml-auto flex gap-1">
            <button type="button" className="trade-button" disabled={!c.legs.length || c.legs.some((leg) => !leg.symbol)} onClick={() => setTicket({ legs: c.legs, units: c.units, shares: c.shares })}>Send to ticket</button>
            <button type="button" className="trade-button" aria-label={`Remove ${c.name}`} onClick={() => updateAnalyze(symbol, (s) => ({ ...s, candidates: s.candidates.filter((x) => x.id !== c.id) }))}>Remove</button>
          </span>
        </div>
        <table className="mt-2 w-full"><tbody>{c.legs.map((leg, index) => <LegRow key={index} leg={leg} chain={chains.get(leg.expiry)} expiries={expiries}
          onChange={(next) => setLeg(c.id, index, next)} onRemove={() => update(c.id, (x) => ({ ...x, legs: x.legs.filter((_, k) => k !== index) }))} />)}</tbody></table>
        <div className="mt-1 flex gap-1">{(["call", "put"] as const).map((type) => <button key={type} type="button" className="trade-button !py-0.5"
          disabled={c.legs.length >= MAX_LEGS} onClick={() => { const leg = atmLeg(c.legs.at(-1)?.expiry ?? front ?? "", type); if (leg) update(c.id, (x) => ({ ...x, legs: [...x.legs, leg] })) }}>
          Add {type}</button>)}</div>
      </article>)}</div>
    </Panel>

    {visible.length > 0 && spot != null && <Panel title="Price slices">
      <Slices spot={spot} candidates={visible} profiles={results} lines={lines} basis={basis} />
    </Panel>}

    <Panel title="Strategy optimizer">
      <StrategyOptimizer symbol={symbol} spot={data.spot ?? null} expiries={expiries}
        onAnalyze={(candidate: StrategyCandidate) => addAnalyzeCandidate(symbol, { name: candidate.name.slice(0, 64), legs: candidateLegs(candidate, symbol), units: 1, shares: candidate.shares })}
        onTicket={(candidate: StrategyCandidate) => setTicket({ legs: candidateLegs(candidate, symbol), units: Math.max(1, candidate.quantity), shares: candidate.shares })} />
    </Panel>
    {ticket && live.trading && <AnalyzeTicket symbol={symbol} legs={ticket.legs} units={ticket.units} shares={ticket.shares} trading={live.trading} onClose={() => setTicket(null)} />}
  </div>
}
