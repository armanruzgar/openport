// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, useState } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import * as connection from "../api/connection"
import { liveState, useLive } from "../api/live"
import { activeAccount, useActiveAccount } from "../lib/active-account"
import { dataSource } from "../lib/data-source"
import { createTokenStore, useWriteToken, writeToken } from "../lib/write-token"
import { account, portfolio, quote, selection, status, trading, risk } from "../test/trading-fixtures"
import { renderTimeout, waitForRender } from "../test/render"
import { AccountSwitcher } from "./AccountSwitcher"
import { OrderTicket } from "./OrderTicket"
import { WatchOnlyNotice, WriteAccess } from "./TradingControls"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const sandboxToken = `sandbox_${"a".repeat(32)}`
const sandboxId = `sbox-${"b".repeat(32)}`
const offered = { enabled: true, idle_seconds: 86400 }
const tokenTrading = { ...trading, write: "token" as const }
let root: Root
let host: HTMLDivElement
let client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  writeToken.set("")
  activeAccount.set("main")
  dataSource.set("live")
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, trading: tokenTrading }, null, "closed"))
  vi.spyOn(api, "orders").mockResolvedValue({ account_version: "17", orders: [] })
  vi.spyOn(api, "account").mockResolvedValue(account)
  vi.spyOn(api, "risk").mockResolvedValue(risk)
  vi.spyOn(api, "fills").mockResolvedValue({ account_version: "17", fills: [] })
  vi.spyOn(api, "portfolio").mockResolvedValue({ ...portfolio, positions: [] })
  vi.spyOn(api, "replay").mockResolvedValue({ replay: null, directory: "", recordings: [], history: [] })
  vi.spyOn(api, "summary").mockRejectedValue(new Error("No summary yet"))
  vi.spyOn(api, "previewOrder").mockRejectedValue(new Error("No preview yet"))
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  host = document.createElement("div")
  document.body.append(host)
  root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, gcTime: Infinity } } })
})
afterEach(async () => {
  await act(async () => root.unmount())
  client.clear()
  host.remove()
  writeToken.set("")
  activeAccount.set("main")
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
})
async function render(node: React.ReactNode) {
  await act(async () => root.render(<QueryClientProvider client={client}>{node}</QueryClientProvider>))
}
function button(text: string) {
  const found = [...host.querySelectorAll("button")].find((item) => item.textContent === text || item.getAttribute("aria-label") === text)
  if (!found) throw new Error(`Missing button ${text}`)
  return found
}
function Harness({ offers = true }: { offers?: boolean }) {
  const saved = useWriteToken()
  const selected = useActiveAccount()
  const [created, setCreated] = useState("")
  return <>
    <WatchOnlyNotice trading={tokenTrading} sandboxes={offers ? offered : undefined} onCreated={setCreated} />
    <output>{selected} {created} {saved ? "token saved" : "no token"}</output>
    <OrderTicket selection={selection} quote={quote} trading={tokenTrading} onClose={() => {}} />
  </>
}
it("offers a sandbox only when advertised, with the operator token action kept", async () => {
  await render(<WatchOnlyNotice trading={tokenTrading} />)
  expect(host.textContent).not.toContain("Try trading with a sandbox account")
  expect(button("Enter write token")).toBeTruthy()
  await render(<WatchOnlyNotice trading={tokenTrading} sandboxes={{ ...offered, enabled: false }} />)
  expect(host.textContent).not.toContain("Try trading with a sandbox account")
  await render(<WatchOnlyNotice trading={tokenTrading} sandboxes={offered} />)
  expect(button("Try trading with a sandbox account")).toBeTruthy()
  expect(button("Enter write token")).toBeTruthy()
  await act(async () => { writeToken.set("operator-secret") })
  expect(host.textContent).not.toContain("Try trading with a sandbox account")
})
it("creates without authorization, stores the secret in session storage, switches accounts and enables the ticket", async () => {
  const fetcher = vi.fn().mockResolvedValue(new Response(JSON.stringify({ account: sandboxId, token: sandboxToken, idle_seconds: 86400, simulated: true }), { status: 201 }))
  vi.stubGlobal("fetch", fetcher)
  await render(<Harness />)
  expect(button("Submit order").disabled).toBe(true)
  await act(async () => { button("Try trading with a sandbox account").click() })
  expect(fetcher).toHaveBeenCalledWith("/api/sandboxes", expect.objectContaining({ method: "POST", body: "{}" }))
  expect(new Headers(fetcher.mock.calls[0]?.[1]?.headers).has("Authorization")).toBe(false)
  expect(sessionStorage.getItem("openport.write-token")).toBe(sandboxToken)
  expect(writeToken.get()).toBe(sandboxToken)
  expect(activeAccount.get()).toBe(sandboxId)
  expect(host.querySelector("output")?.textContent).toContain(`${sandboxId} ${sandboxId} token saved`)
  expect(button("Submit order").disabled).toBe(false)
  expect(window.location.href).not.toContain(sandboxToken)
})
it("drops a refused sandbox secret and offers a fresh sandbox without repeating the failed request", async () => {
  const fetcher = vi.fn().mockResolvedValue(new Response(JSON.stringify({ error: { code: "SANDBOX_EXPIRED", message: "Expired" } }), { status: 403 }))
  vi.stubGlobal("fetch", fetcher)
  writeToken.set(sandboxToken, sandboxId)
  activeAccount.set(sandboxId)
  await render(<Harness />)
  await act(async () => { await expect(api.status()).rejects.toMatchObject({ code: "SANDBOX_EXPIRED" }) })
  expect(writeToken.get()).toBe("")
  expect(sessionStorage.getItem("openport.write-token")).toBeNull()
  expect(activeAccount.get()).toBe("main")
  expect(button("Try trading with a sandbox account")).toBeTruthy()
  expect(button("Submit order").disabled).toBe(true)
  expect(fetcher).toHaveBeenCalledTimes(1)
})
it("does not erase a newer token when an old request is refused", async () => {
  let resolve!: (response: Response) => void
  vi.stubGlobal("fetch", vi.fn(() => new Promise<Response>((done) => { resolve = done })))
  writeToken.set(sandboxToken, sandboxId)
  const pending = api.status()
  writeToken.set("operator-new")
  resolve(new Response(JSON.stringify({ error: { code: "SANDBOX_EXPIRED", message: "Expired" } }), { status: 403 }))
  await expect(pending).rejects.toMatchObject({ code: "SANDBOX_EXPIRED" })
  expect(writeToken.get()).toBe("operator-new")
})
it("does not discard an operator token or a sandbox token on ordinary scope or rate errors", async () => {
  for (const saved of ["operator-secret", sandboxToken]) {
    writeToken.set(saved, saved === sandboxToken ? sandboxId : "")
    vi.stubGlobal("fetch", vi.fn().mockResolvedValue(new Response(JSON.stringify({ error: { code: "SCOPE_REQUIRED", message: "Not permitted" } }), { status: 403 })))
    await expect(api.status()).rejects.toMatchObject({ code: "SCOPE_REQUIRED" })
    expect(writeToken.get()).toBe(saved)
  }
})
it("keeps token entry working for the operator", async () => {
  await render(<WriteAccess trading={tokenTrading} />)
  await act(async () => { button("Enter write token").click() })
  const field = host.querySelector<HTMLInputElement>('input[type="password"]')!
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(field, "operator-secret")
    field.dispatchEvent(new Event("input", { bubbles: true }))
  })
  await act(async () => { host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })) })
  expect(writeToken.get()).toBe("operator-secret")
  expect(button("Manage write token")).toBeTruthy()
})
it("labels the idle lifetime in the account area and omits new-account and replay choices", async () => {
  writeToken.set(sandboxToken, sandboxId)
  client.setQueryData(["replay-listing"], { history: [{ id: "old-run", file: "cached replay", demo: true, result: "finished" }] })
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, trading: tokenTrading,
    accounts: [{ id: sandboxId, name: "Sandbox", trading: tokenTrading, sandbox_idle_seconds: 86400 }] }, null, "closed", 1, sandboxId))
  await render(<AccountSwitcher />)
  expect(host.textContent).toContain("Sandbox · removed after 24 h unused")
  expect(host.textContent).not.toContain("New account")
  expect(host.textContent).not.toContain("cached replay")
  expect(api.replay).not.toHaveBeenCalled()
})
it("keeps a sandbox token usable in memory when session storage is denied", () => {
  const store = createTokenStore(() => { throw new Error("storage denied") })
  expect(store.set(sandboxToken, sandboxId)).toBe(false)
  expect(store.get()).toBe(sandboxToken)
  expect(store.sandboxAccount()).toBe(sandboxId)
  store.set("")
  expect(store.get()).toBe("")
})
it("shows capacity errors without saving a token or switching accounts", async () => {
  vi.stubGlobal("fetch", vi.fn().mockResolvedValue(new Response(JSON.stringify({ error: { code: "SANDBOX_CAPACITY", message: "All sandbox accounts are in use. Try again later." } }), { status: 429 })))
  await render(<WatchOnlyNotice trading={tokenTrading} sandboxes={offered} />)
  await act(async () => { button("Try trading with a sandbox account").click() })
  expect(host.querySelector('[role="alert"]')?.textContent).toContain("All sandbox accounts are in use")
  expect(writeToken.get()).toBe("")
  expect(activeAccount.get()).toBe("main")
})

it("restores sandbox identity per session and clears it when an operator replaces the token", () => {
  const values = new Map<string, string>()
  const storage = () => ({ getItem: (key: string) => values.get(key) ?? null,
    setItem: (key: string, value: string) => { values.set(key, value) }, removeItem: (key: string) => { values.delete(key) } })
  createTokenStore(storage).set(sandboxToken, sandboxId)
  const restored = createTokenStore(storage)
  expect(restored.get()).toBe(sandboxToken)
  expect(restored.sandboxAccount()).toBe(sandboxId)
  restored.set("sandbox_operator-token")
  expect(restored.sandboxAccount()).toBe("")
  expect(createTokenStore(storage).sandboxAccount()).toBe("")
})

it("restores the sandbox account after a reload even when another tab last selected main", async () => {
  const actual = await vi.importActual<typeof import("../api/live")>("../api/live")
  vi.spyOn(connection, "connectLive").mockImplementation((_url, _client, _tick, onConnection) => {
    onConnection("closed")
    return () => {}
  })
  writeToken.set(sandboxToken, sandboxId)
  activeAccount.set("main")
  dataSource.set("replay")
  vi.spyOn(api, "status").mockResolvedValue({ ...status, sandboxes: offered, trading: tokenTrading,
    accounts: [{ id: sandboxId, name: "Sandbox", trading: tokenTrading, sandbox_idle_seconds: 86400 }] })
  function Probe() {
    const live = actual.useLive()
    return <output>{live.account} {live.trading?.enabled ? "ticket available" : "waiting"}</output>
  }
  await render(<actual.LiveProvider><Probe /></actual.LiveProvider>)
  // The socket never opens here, so the page also says the connection dropped.
  await waitForRender(() => expect(host.querySelector("output")?.textContent).toBe(`${sandboxId} ticket available`))
  expect(dataSource.get()).toBe("live")
  expect(api.replay).not.toHaveBeenCalled()
}, renderTimeout)
it("recovers the live terminal from a restarted server with a public status request and a new offer", async () => {
  const actual = await vi.importActual<typeof import("../api/live")>("../api/live")
  vi.spyOn(connection, "connectLive").mockImplementation((_url, _client, _tick, onConnection) => {
    onConnection("closed")
    return () => {}
  })
  writeToken.set(sandboxToken, sandboxId)
  activeAccount.set(sandboxId)
  let refused = 0
  vi.stubGlobal("fetch", vi.fn(async (_path: string, init: RequestInit) => {
    if (new Headers(init.headers).has("Authorization")) {
      ++refused
      return new Response(JSON.stringify({ error: { code: "SANDBOX_EXPIRED", message: "Expired" } }), { status: 403 })
    }
    return new Response(JSON.stringify({ ...status, trading: tokenTrading, sandboxes: offered }), { status: 200 })
  }))
  function Probe() {
    const live = actual.useLive()
    return <WatchOnlyNotice trading={live.trading} sandboxes={live.status?.sandboxes} />
  }
  await render(<actual.LiveProvider><Probe /></actual.LiveProvider>)
  await waitForRender(() => expect(button("Try trading with a sandbox account")).toBeTruthy())
  expect(writeToken.get()).toBe("")
  expect(activeAccount.get()).toBe("main")
  expect(button("Try trading with a sandbox account")).toBeTruthy()
  expect(refused).toBe(1)
}, renderTimeout)
