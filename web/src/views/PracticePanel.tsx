import { useMemo, useState } from "react"
import { useQuery } from "@tanstack/react-query"
import { useActionApi } from "../api/action-client"
import { userReplayChange } from "../api/destination"
import { useTrades } from "../api/trading"
import { useLive } from "../api/live"
import type { ReplayListing, ScenarioPack } from "../api/types"
import { TradingError } from "../components/TradingControls"
import { Panel } from "../components/ui"
import { newYorkDate, tradeBuckets } from "../lib/journal"
import { practiceQueue, practiceStreak } from "../lib/practice"
import { useReplayControls } from "./ReplayView"

export function PracticePanel({ listing, refresh }: { listing?: ReplayListing; refresh: () => void }) {
  const api = useActionApi()
  const live = useLive()
  const controls = useReplayControls()
  const [expanded, setExpanded] = useState(false)
  const packs = useQuery({ enabled: expanded, queryKey: ["scenario-packs"], queryFn: ({ signal }) => api.scenarioPacks(signal) })
  const [text, setText] = useState("")
  const [error, setError] = useState<unknown>()
  const [pending, setPending] = useState(false)
  const journal = useTrades("all")
  const edges = useMemo(() => tradeBuckets(journal.data?.trades ?? [], "underlying"), [journal.data])
  const today = newYorkDate(new Date().toISOString())!.date
  const queue = useMemo(() => practiceQueue(listing?.demos ?? [], listing?.history ?? [], today, undefined, edges), [listing?.demos, listing?.history, today, edges])
  async function importPack() {
    setPending(true); setError(undefined)
    try { await api.importScenarioPack(text, listing?.write ?? "disabled"); setText(""); await packs.refetch(); refresh() }
    catch (e) { setError(e) } finally { setPending(false) }
  }
  async function startPack(pack: ScenarioPack) {
    setPending(true); setError(undefined)
    try {
      await userReplayChange(() => api.startScenarioPack(pack.scenario.id, listing?.write ?? "disabled"))
      live.switchSource("replay", "user"); refresh()
    } catch (e) { setError(e) } finally { setPending(false) }
  }
  return <>
    <Panel title="Today’s practice" actions={<span className="text-xs">{practiceStreak(listing?.history ?? [], today)} day streak</span>}>
      <p className="mb-3 text-xs text-muted">Lower recent scores return sooner: 1, 3 or 7 days after completion. Losing underlying groups in the selected Journal (at least three trades) return after one day. New seeds every visit. Streaks use actual completion dates in New York.</p>
      {queue.length ? queue.map(item => <div key={item.scenario} className="flex flex-wrap items-center gap-3 py-2 text-xs"><span>{item.title}</span><span className="text-muted">{item.score == null ? "New drill" : `Last score ${item.score.toFixed(0)}/100`} · seed {item.seed}{item.weak.length ? ` · weak ${item.weak.join(", ")} group` : ""}</span>
        <button className="trade-button" disabled={controls.pending || controls.blocked} onClick={() => void controls.start({ demo: item.scenario }, 10, () => { live.switchSource("replay", "user"); refresh() }, { seed: item.seed, plan: item.plan, paused: true })}>Practise</button></div>) : <p className="text-xs text-muted">Nothing due today.</p>}
      <TradingError error={controls.error} />
    </Panel>
    <Panel title="Scenario packs">
      <p className="text-xs text-muted">Import scenario JSON, objectives, seed and plan together. A challenge fixes those inputs; the verification receipt includes its definition.</p>
      <label className="trade-label mt-2">Pack JSON<textarea className="trade-input min-h-24" value={text} onChange={e => setText(e.target.value)} maxLength={65_536} /></label>
      <label className="block text-xs">Load JSON file<input type="file" accept="application/json,.json" onChange={e => {
        const file = e.target.files?.[0]; if (!file) return
        if (file.size > 65_536) { setError(new Error("Scenario packs are at most 64 KiB")); return }
        void file.text().then(setText).catch(setError)
      }} /></label>
      <button className="trade-button my-2" disabled={!text || pending || controls.blocked} onClick={() => void importPack()}>Import pack</button>
      <TradingError error={error ?? packs.error} />
      <details onToggle={e => setExpanded(e.currentTarget.open)}><summary className="text-xs">Export or run a pack</summary>{packs.data?.packs?.map(pack => <div key={pack.scenario.id} className="flex flex-wrap items-center gap-2 py-2 text-xs"><span>{pack.challenge?.title ?? pack.scenario.title} · seed {pack.seed} · {pack.plan}</span>
        <button className="trade-button" onClick={() => void api.downloadScenarioPack(pack.scenario.id).catch(setError)}>Export JSON</button><button className="trade-button" disabled={pending || controls.blocked} onClick={() => void startPack(pack)}>{pack.challenge ? "Start challenge" : "Run pack"}</button></div>)}</details>
    </Panel>
  </>
}
