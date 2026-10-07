import { useActionApi } from "../api/action-client"
import { PendingSettingsNotice } from "../components/PendingSettingsNotice"
import { useQueryClient } from "@tanstack/react-query"
import { useState, type ReactNode } from "react"
import { useBriefMarket } from "../api/brief"

import { useLive } from "../api/live"
import { useAccount, useOpenOrders, usePortfolio, useRefreshTrading, useRisk, useTrades, useTradingSession } from "../api/trading"
import type { Account, DayNote, Guardrails, Limits, Risk, TradesResponse, TradingStatus } from "../api/trading-types"
import { Badge, PageHeader, Panel } from "../components/ui"
import { AutoPlaybookIndicator, StagedOrders } from "../components/Playbooks"
import { TradingError, WriteAccess, writeBlocked } from "../components/TradingControls"
import { invertedTerm, marketDate, notePhase, overnightMove, lossAllowance, orderDeadlines, positionDeadlines, volChange, weekMove, type BriefMove } from "../lib/brief"
import { saveBriefLevels, useBriefLevels } from "../lib/brief-preferences"
import { barClock } from "../lib/candles"
import { fixed, isNum, money, price } from "../lib/format"
import { contractLabel, orderLabel } from "../lib/journal"
import { formatMoney, subtractMoney } from "../lib/trading"
import { metricMark, volPoints } from "../lib/volatility"
import { useWriteToken } from "../lib/write-token"

type Market = ReturnType<typeof useBriefMarket>
const reasonText = (reason: string | null | undefined, fallback: string) => reason?.replaceAll("_", " ") || fallback
const signed = (value: number | null | undefined) => isNum(value) ? `${value > 0 ? "+" : ""}${fixed(value, 2)}` : "—"
const stamp = (time: string | null | undefined) => time && Number.isFinite(Date.parse(time))
  ? `${marketDate(Date.parse(time))} ${barClock(Date.parse(time) / 1000)} ET` : "time unavailable"
function Missing({ reason }: { reason: string }) { return <span className="text-faint">— {reason}</span> }
function Rows({ children }: { children: ReactNode }) { return <dl className="space-y-1 text-xs">{children}</dl> }
function Row({ label, children }: { label: string; children: ReactNode }) {
  return <div className="flex flex-wrap justify-between gap-x-3 gap-y-0.5"><dt className="text-muted">{label}</dt><dd className="tabular [overflow-wrap:anywhere]">{children}</dd></div>
}
function Value({ value, reason = "unavailable" }: { value: number | null | undefined; reason?: string }) {
  return isNum(value) ? <>{price(value)}</> : <Missing reason={reason} />
}
function Move({ move, spot }: { move: BriefMove; spot: number | null | undefined }) {
  return <>
    <div className="text-lg tabular">{isNum(move.points) ? <>±{price(move.points)} pts{metricMark(move)}</> : <Missing reason={reasonText(move.reason, "move unavailable")} />}</div>
    <div className="mt-0.5 text-[11px] text-muted">
      {isNum(move.percent) && <>{fixed(move.percent, 2)}% · </>}
      {isNum(spot) && isNum(move.points) ? `${price(spot - move.points)}–${price(spot + move.points)}` : "bands unavailable"}
      {move.shared && " · shared variance"}
    </div>
  </>
}

export function BriefMarketPanels({ market }: { market: Market }) {
  const { summary, volatility, prior, priorDay, closeRow, overnight, move, day, now } = market
  const data = summary.data, vol = volatility.data
  const showLevels = useBriefLevels()
  const change = overnightMove(data?.spot, prior?.c)
  const week = weekMove(vol, day, data?.spot)
  const mfiv = vol?.mfiv.constant.find(row => row.days === 30)
  const atm = vol?.atm.constant.find(row => row.days === 30)
  const spotSource = data?.spot_source === "parity" ? "≈ parity-inferred spot" : data?.spot_source === "quote" ? "quoted spot" : "spot source unavailable"
  const unavailable = !market.active ? "not subscribed" : summary.isError ? "summary request failed" : "no snapshot yet"
  const historyReason = market.series.isError ? "series request failed" : !market.series.data?.rows.length ? "no series history yet" : "no prior-close series row"
  return <>
    <Panel title="Where it is">
      <div className="flex flex-wrap items-baseline gap-x-2"><span className="text-xl font-medium tabular">{data?.spot_source === "parity" && "≈ "}<Value value={data?.spot} reason={unavailable} /></span><span className="text-[11px] text-muted">{spotSource}</span></div>
      <p className="mb-3 mt-1 text-xs tabular">{change ? <>{signed(change.points)} pts ({signed(change.percent)}%) <span className="text-muted">{day && (day > (market.currentDate ?? "") || barClock(now / 1000) < "09:30") ? "overnight / pre-market" : "since close"}</span></> : <Missing reason="spot or prior close unavailable" />}</p>
      <Rows>
        <Row label={`Prior close${priorDay ? ` · ${priorDay}` : ""}`}><Value value={prior?.c} reason="no prior-session bar" /></Row>
        <Row label="Prior high / low">{prior ? `${price(prior.h)} / ${price(prior.l)}` : <Missing reason="no prior-session bar" />}</Row>
        <Row label="Overnight high / low">{overnight ? `${price(overnight.high)} / ${price(overnight.low)}` : <Missing reason="no overnight candles" />}</Row>
      </Rows>
      <p className="mt-2 text-[11px] text-faint">Observed bars, not guaranteed full-session coverage. Overnight: 20:15–09:30 ET{overnight ? ` · ${overnight.bars} minute bars` : ""}. {Number.isFinite(now) && `As of ${data?.as_of ? stamp(data.as_of) : barClock(now / 1000) + " ET"}.`}</p>
    </Panel>
    <Panel title="What’s priced">
      <div className="text-[11px] uppercase text-muted">Today remaining · 1σ</div><Move move={move} spot={data?.spot} />
      <div className="mt-3 text-[11px] uppercase text-muted">Rest of week · includes today</div><Move move={week} spot={data?.spot} />
      <p className="mt-2 text-[11px] text-faint">1σ bands around spot. Today: expiry or allocated session variance. Week: summed session variances.</p>
      <div className="mt-2 border-t border-border pt-2 text-xs"><span className="text-muted">Today’s events: </span>{vol?.implied_moves.sessions.find(row => row.date === day)?.label ?? "No supplied label for this session."}</div>
    </Panel>
    <Panel title="Volatility" className="xl:col-span-2 xl:row-start-2">
      {!vol ? <Missing reason={volatility.isError ? "volatility request failed" : "no volatility snapshot"} /> : <>
        <div className="grid gap-x-6 gap-y-2 sm:grid-cols-2">
          <Rows>
            {([["Model-free IV · 30d", mfiv?.vol, closeRow?.mfiv30, mfiv], ["ATM IV · 30d", atm?.vol, closeRow?.atm30, atm],
              ["25Δ risk reversal · 30d", vol.skew.delta25.rr, closeRow?.rr25, vol.skew]] as const).map(([label, value, previous, flags]) => <Row key={label} label={label}>
              {isNum(value) ? <>{volPoints(value)}{metricMark(flags)} <span className="text-muted">({isNum(previous) ? `${signed(volChange(value, previous))} Δ` : "Δ unavailable"})</span></> : <Missing reason={reasonText(flags?.reason, "not bracketed")} />}
            </Row>)}
            <Row label="IV rank / percentile">{isNum(vol.iv_rank) || isNum(vol.iv_percentile) ? `${isNum(vol.iv_rank) ? fixed(vol.iv_rank * 100, 1) + "%" : "—"} / ${isNum(vol.iv_percentile) ? fixed(vol.iv_percentile * 100, 1) + "%" : "—"}` : <Missing reason="insufficient IV history" />}</Row>
          </Rows>
          <Rows>
            {[10, 21].map(sessions => {
              const rv = vol.realized.windows.find(row => row.sessions === sessions)?.close_to_close
              return <Row key={sessions} label={`RV ${sessions} / IV 30d`}>{isNum(rv?.vol) ? `${volPoints(rv.vol)} / ${volPoints(mfiv?.vol)}` : <Missing reason={reasonText(rv?.reason, "insufficient daily bars")} />}</Row>
            })}
            <Row label="VRP · IV − RV21 / ratio">{isNum(vol.vrp.spread) ? `${signed(vol.vrp.spread)} / ${volPoints(vol.vrp.ratio)}${metricMark(vol.vrp)}` : <Missing reason={reasonText(vol.vrp.reason, "IV or RV missing")} />}</Row>
            {([["9d / 30d", vol.term.mfiv9_30], ["30d / 93d", vol.term.mfiv30_93]] as const).map(([label, value]) => <Row key={label} label={label}>{isNum(value) ? <span className={invertedTerm(value) ? "text-warn" : ""}>{fixed(value, 3)}{metricMark(vol.term)}{invertedTerm(value) && " · backwardation"}</span> : <Missing reason="tenors not bracketed" />}</Row>)}
          </Rows>
        </div>
        <p className="mt-2 text-[11px] text-muted">{vol.history_sessions != null ? `${vol.history_sessions} of ${vol.history_basis?.window ?? 252} sessions` : "History basis unavailable"} · {vol.history_basis?.current === "own_mfiv" ? "model-free IV" : vol.history_basis?.current === "own_atm" ? "ATM fallback" : "current IV unavailable"}
          {vol.proxy?.used && ` · ${vol.history_basis?.proxy_sessions ?? "—"} ${vol.proxy.name ?? "index"} proxy sessions`}. RV through {vol.realized.daily_as_of ?? "—"}.</p>
        <p className="mt-1 text-[11px] text-faint">{closeRow ? `Δ vs ${priorDay} close row${closeRow.sample_time ? `, sampled ${barClock(closeRow.sample_time)} ET` : ""}.` : historyReason + "."} Vol and skew in vol points. † Truncated strip. ≈ Proxy or interpolation. RR is call minus put IV.</p>
      </>}
    </Panel>
    <Panel title="Positioning" className="xl:row-start-3">
      <Rows>
        <Row label="Total GEX">{isNum(data?.exposure.gex) ? `${data.exposure.gex > 0 ? "Positive" : data.exposure.gex < 0 ? "Negative" : "Zero"} · ${money(data.exposure.gex)}` : <Missing reason="no usable exposure" />}</Row>
        <Row label="Gamma flip"><Value value={data?.exposure.gamma_flip} reason="no model crossing" /></Row>
        <Row label="Call wall"><Value value={data?.exposure.call_wall} reason="no usable exposure" /></Row>
        <Row label="Put wall"><Value value={data?.exposure.put_wall} reason="no usable exposure" /></Row>
      </Rows>
      <p className="mt-3 text-[11px] text-faint">Exposure is a modelling convention: dealers long calls, short puts. Actual dealer positioning is unknown; the opposite convention reverses the sign.</p>
    </Panel>
    <Panel title="Key levels" className="xl:row-start-3" actions={<label className="flex items-center gap-1.5 text-xs text-muted"><input type="checkbox" checked={showLevels} onChange={event => saveBriefLevels(event.target.checked)} />Draw on Trade</label>}>
      <div className="max-h-36 overflow-y-auto"><Rows>{market.levels.length ? market.levels.map(level => <Row key={level.price} label={level.label}>{price(level.price)}</Row>) : <Missing reason="no levels available" />}</Rows></div>
      <p className="mt-2 text-[11px] text-faint">Missing levels are omitted. {move.proxy && "Today’s bands use a proxy. "}{move.truncated && "Today’s bands use a truncated strip. "}Saved in this browser.</p>
    </Panel>
  </>
}

function guardrailRows(guardrails: Guardrails) {
  return [
    ["Soft floor equity", Number(guardrails.soft_floor) > 0 ? formatMoney(guardrails.soft_floor) : "Off"],
    ["Soft floor reserve", guardrails.soft_floor_percent ? `${guardrails.soft_floor_percent}% of drawdown` : "Off"],
    ["Opening orders", guardrails.max_opening_trades || "Off"],
    ["Cooldown", guardrails.cooldown_minutes ? `${guardrails.cooldown_minutes} min · loss > ${formatMoney(guardrails.cooldown_loss)} or stop-out` : "Off"],
    ["Profit lock", Number(guardrails.profit_lock) > 0 ? formatMoney(guardrails.profit_lock) : "Off"],
  ] as const
}
function pendingLimits(active: Limits, pending: Limits) {
  return Object.entries(pending).filter(([key, value]) => JSON.stringify(value) !== JSON.stringify(active[key as keyof Limits]))
    .flatMap(([key, value]) => value != null && typeof value === "object"
      ? Object.entries(value).map(([name, cap]) => [`${key} ${name}`.replaceAll("_", " "), String(cap)])
      : [[key.replaceAll("_", " "), String(value)]])
}
export function BriefAccountPanel({ account, risk }: { account?: Account; risk?: Risk }) {
  const allowance = lossAllowance(account, risk)
  const evaluation = account?.evaluation
  const state = risk?.guardrail_state ?? account?.guardrail_state
  const guardrails = risk?.guardrails ?? account?.guardrails
  const complete = evaluation?.valuation_complete
  return <Panel title={account?.rules.plan ?? "Account plan"} className="xl:col-start-3 xl:row-start-1" actions={evaluation && <Badge>{evaluation.enabled ? `${evaluation.status} · attempt ${evaluation.attempt}` : "Practice"}</Badge>}>
    <Rows>
      <Row label="Target equity">{evaluation?.target_equity ? formatMoney(evaluation.target_equity) : <Missing reason={account ? "no target" : "account unavailable"} />}</Row>
      <Row label="Floor / room">{evaluation?.floor != null ? `${formatMoney(evaluation.floor)} / ${complete ? formatMoney(evaluation.drawdown_buffer) : "unmarked"}` : <Missing reason={account ? "no plan floor" : "account unavailable"} />}</Row>
      <Row label="Soft floor / room">{state?.soft_floor != null ? `${formatMoney(state.soft_floor)} / ${complete ? formatMoney(subtractMoney(evaluation?.equity, state.soft_floor)) : "unmarked"}` : <Missing reason={state ? "not configured" : "guardrails unavailable"} />}</Row>
      <Row label="Closest floor room">{evaluation?.closest_floor != null ? <span title={stamp(evaluation.closest_floor_at)}>{formatMoney(evaluation.closest_floor)} · {stamp(evaluation.closest_floor_at)}</span> : <Missing reason="not recorded" />}</Row>
    </Rows>
    <div className="mt-2 border-t border-border pt-2"><div className="flex flex-wrap items-baseline justify-between gap-x-2"><span className="text-[11px] uppercase text-muted">Today’s loss allowance</span><span className="text-lg tabular">{allowance.value != null ? formatMoney(allowance.value) : "—"}</span></div><p className="text-[11px] text-muted">{allowance.reason}{allowance.value != null && " · smallest active room"}</p></div>
    {guardrails && <p className="mt-2 text-[11px] text-muted">Opening orders {state?.opening_trades ?? "—"}/{guardrails.max_opening_trades || "unlimited"} · cooldown {guardrails.cooldown_minutes ? `${guardrails.cooldown_minutes}m` : "off"} · profit lock {Number(guardrails.profit_lock) > 0 ? formatMoney(guardrails.profit_lock) : "off"}</p>}
    <details className="mt-2 text-xs"><summary className="cursor-pointer text-muted">Guardrails & limits{risk?.pending_limits || risk?.pending_guardrails ? " · changes pending" : ""}{!!state?.latched.length || risk?.kill.latched ? " · reduce-only" : ""}</summary>
      <div className="mt-2 space-y-2">
        {allowance.parts.map(part => <p key={part.label}>{part.label}: {formatMoney(part.value)}</p>)}
        {state && <p>{state.opening_trades} opening orders · {state.latched.join(", ") || "no guardrail latch"}{state.cooldown_seconds > 0 && ` · ${state.cooldown_seconds}s until ${stamp(state.cooldown_until)}`}</p>}
        {risk?.kill.latched && <p className="text-warn">Reduce-only: {risk.kill.reason ?? "kill switch"}</p>}
        {guardrails ? <Rows>{guardrailRows(guardrails).map(([label, value]) => <Row key={label} label={label}>{value}</Row>)}</Rows> : <Missing reason="guardrails unavailable" />}
        {!!risk?.breach?.underlyings.length && (risk.breach.room != null || risk.breach.soft_room != null) && <div><p className="text-muted">{risk.breach.room != null ? "Floor" : "Soft floor"} breach levels · model estimates</p>{risk.breach.underlyings.map(row => <p key={row.underlying}>{row.underlying}: {row.complete ? `down ${row.down ? price(row.spot + row.down.points) : "—"} / up ${row.up ? price(row.spot + row.up.points) : "—"}` : "incomplete valuation"}</p>)}<p className="text-[11px] text-faint">Unchanged volatility, one underlying at a time. Missing crossings are not a safety bound.</p></div>}
        <PendingSettingsNotice requiresReset={risk?.pending_requires_reset} />
        {risk?.pending_guardrails && <div className="text-warn"><p className="mb-1">Pending guardrails · next trading day</p><Rows>{guardrailRows(risk.pending_guardrails).map(([label, value]) => <Row key={label} label={label}>{value}</Row>)}</Rows></div>}
        {risk?.pending_limits && <div className="text-warn"><p className="mb-1">Pending limits · next trading day</p><Rows>{pendingLimits(risk.limits, risk.pending_limits).map(([label, value]) => <Row key={label!} label={label!}>{value}</Row>)}</Rows></div>}
      </div>
    </details>
  </Panel>
}

export function BriefNote({ day, phase, note, trading }: { day: string; phase: "plan" | "review"; note?: DayNote; trading: TradingStatus }) {
  const api = useActionApi()
  const [draft, setDraft] = useState<Partial<Pick<DayNote, "plan" | "review">>>({})
  const [busy, setBusy] = useState(false)
  const [saved, setSaved] = useState(false)
  const [error, setError] = useState<unknown>()
  const token = useWriteToken(), refresh = useRefreshTrading(), sameSession = useTradingSession()
  const client = useQueryClient(), { accountScope } = useLive()
  const value = draft[phase] ?? note?.[phase] ?? ""
  async function save() {
    if (busy || writeBlocked(trading, token)) return
    setBusy(true); setSaved(false); setError(undefined)
    const next = { plan: draft.plan ?? note?.plan ?? "", review: draft.review ?? note?.review ?? "" }
    try {
      const result = await api.annotateDay(day, next, trading.write)
      if (sameSession()) {
        client.setQueriesData<TradesResponse>({ queryKey: ["trading", accountScope, "trades"] }, previous => previous
          ? { ...previous, day_notes: { ...previous.day_notes, [day]: result.note } } : previous)
        setDraft({}); setSaved(true); void refresh()
      }
    } catch (failure) { if (sameSession()) setError(failure) }
    finally { if (sameSession()) setBusy(false) }
  }
  return <form className="space-y-2" onSubmit={event => { event.preventDefault(); void save() }}>
    <label className="trade-label">{phase === "review" ? "Review after the close" : "Plan before the open"} · {day}
      <textarea className="trade-input min-h-20" maxLength={2000} value={value} disabled={busy || writeBlocked(trading, token)} onChange={event => { setDraft(current => ({ ...current, [phase]: event.target.value })); setSaved(false) }} /></label>
    {phase === "review" && <p className="max-h-12 overflow-y-auto whitespace-pre-wrap text-xs text-muted">Plan: {draft.plan ?? note?.plan ?? "No plan recorded."}</p>}
    <WriteAccess trading={trading} /><TradingError error={error} />
    <button type="submit" className="trade-button" disabled={busy || writeBlocked(trading, token)}>{busy ? "Saving…" : `Save ${phase}`}</button>
    {saved && <span role="status" className="ml-2 text-xs text-muted">Saved</span>}
  </form>
}

export function BriefView({ symbol }: { symbol: string | null }) {
  const live = useLive()
  const market = useBriefMarket(symbol ?? "")
  const account = useAccount(), risk = useRisk(), portfolio = usePortfolio(), orders = useOpenOrders(), trades = useTrades()
  const { day } = market
  const simulated = live.status?.provider.simulated || (live.source !== "live" && live.replay?.demo)
  const phase = day ? notePhase(day, market.now, market.volatility.data) : "plan"
  return <div className="space-y-3">
    <PageHeader title="Brief" subtitle={<>{symbol ?? "No subscribed symbol"} · {day ?? "Market date unavailable"} · market-data time {Number.isFinite(market.now) && `${barClock(market.now / 1000)} ET`}</>}>
      {simulated && <Badge tone="warn">Simulated prices</Badge>}{live.source === "replay" && <Badge>Replay</Badge>}
    </PageHeader>
    <AutoPlaybookIndicator />
    <StagedOrders />
    <div className="grid items-start gap-3 md:grid-cols-2 xl:grid-cols-3">
      <BriefMarketPanels market={market} />
      {live.trading ? <>
        <BriefAccountPanel account={account.data} risk={risk.data} />
        <Panel title="What’s open" className="xl:col-start-3 xl:row-start-2">
          <div className="max-h-48 space-y-2 overflow-y-auto text-xs">
            {portfolio.data ? <>
              <p className="text-muted">{portfolio.data.positions.length} option positions · {portfolio.data.stocks?.length ?? 0} share positions</p>
              {portfolio.data.positions.map(position => <div key={position.symbol}>
                <div>{position.quantity} × {contractLabel(position)}</div>
                <div className="text-[11px] text-muted">{positionDeadlines(position, account.data?.rules.expiry_cutoff_seconds ?? 0, day).map(event => `${event.label} ${barClock(event.time / 1000)} ET`).join(" · ") || (position.expiry === day ? "Expiry today · time unavailable" : `Expiry ${position.expiry}`)}{!position.last_trade_time && " · auto-close time unavailable"}{position.awaiting_settlement && " · awaiting settlement"}</div>
              </div>)}
              {portfolio.data.stocks?.map(position => <p key={position.symbol}>{position.shares} shares {position.symbol}</p>)}
            </> : <Missing reason={portfolio.isError ? "positions request failed" : "positions unavailable"} />}
            <div className="border-t border-border pt-2">
              {orders.data ? <><p className="text-muted">{orders.data.orders.length} working orders</p>{orders.data.orders.map(order => {
                const deadlines = orderDeadlines(order, portfolio.data?.positions ?? [], market.summary.data, account.data?.rules.expiry_cutoff_seconds ?? 0, day)
                return <div key={order.id} className="mt-1"><div>{order.remaining_quantity} × {orderLabel(order)} · {order.status}</div><div className="text-[11px] text-muted">{order.time_in_force.toUpperCase()} · order deadline {order.day_end ? stamp(order.day_end) : "unavailable"}</div>
                  <div className="text-[11px] text-muted">{deadlines.events.map(event => `${event.label} ${barClock(event.time / 1000)} ET`).join(" · ")}{!deadlines.complete && " · contract deadlines unavailable"}</div></div>
              })}</> : <Missing reason={orders.isError ? "orders request failed" : "orders unavailable"} />}
            </div>
          </div>
        </Panel>
        <Panel title={phase === "review" ? "Day review" : "Day plan"} className="xl:col-start-3 xl:row-start-3">
          {day && trades.data ? <BriefNote key={`${live.accountScope}/${day}`} day={day} phase={phase} note={trades.data.day_notes?.[day]} trading={live.trading} /> : <Missing reason={!day ? "market date unavailable" : trades.isError ? "day notes request failed" : "loading day notes"} />}
        </Panel>
      </> : <Panel title="Account"><Missing reason="paper trading is not enabled" /></Panel>}
    </div>
  </div>
}
