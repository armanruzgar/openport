// @vitest-environment jsdom
import { act, StrictMode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { updateInterval, updates } from "../lib/updates"
import { status } from "../test/trading-fixtures"
import { UpdateNotice, UpdateSettings } from "./UpdateNotice"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))

let root: Root
let host: HTMLDivElement
let fetcher: ReturnType<typeof vi.fn<typeof fetch>>
const render = (settings = true) => act(async () => root.render(<StrictMode>{settings && <UpdateSettings />}<UpdateNotice /></StrictMode>))
const optIn = () => act(async () => host.querySelector<HTMLInputElement>("input")!.click())
const release = (version: string) => new Response(JSON.stringify({ tag_name: version, draft: false, prerelease: false }))

beforeEach(() => {
  vi.useFakeTimers()
  vi.setSystemTime(Date.UTC(2026, 8, 27))
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(navigator, "locks", { configurable: true, value: { request: async (_name: string, check: () => Promise<void>) => check() } })
  fetcher = vi.fn<typeof fetch>(async () => release("v0.4.0"))
  vi.stubGlobal("fetch", fetcher)
  localStorage.clear()
  updates.refresh()
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, version: "0.3.0" }, null, "open"))
  host = document.createElement("div")
  document.body.append(host)
  root = createRoot(host)
})
afterEach(async () => {
  await act(async () => root.unmount())
  host.remove()
  localStorage.clear()
  updates.refresh()
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

describe("update settings and notice", () => {
  it("starts unchecked and only requests a release after the user opts in", async () => {
    await render()
    expect(host.querySelector<HTMLInputElement>("input")?.checked).toBe(false)
    expect(host.textContent).toContain("Running version: 0.3.0")
    expect(fetcher).not.toHaveBeenCalled()
    await optIn()
    expect(fetcher).toHaveBeenCalledTimes(1)
    expect(host.querySelector("[role=status]")?.textContent).toContain("OpenPort v0.4.0 is available")
    expect(host.querySelector("a")?.href).toBe("https://github.com/armanruzgar/openport/releases/tag/v0.4.0")
    expect(host.querySelector("a")?.rel).toBe("noopener noreferrer")
    await optIn()
    expect(host.querySelector("[role=status]")).toBeNull()
    await act(async () => { await vi.advanceTimersByTimeAsync(updateInterval) })
    expect(fetcher).toHaveBeenCalledTimes(1)
  })

  it.each(["0.4.0", "0.5.0", undefined])("stays hidden for current version %s", async (version) => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, version }, null, "open"))
    await render()
    await optIn()
    expect(host.querySelector("[role=status]")).toBeNull()
    expect(fetcher).toHaveBeenCalledTimes(version ? 1 : 0)
  })

  it("remembers dismissal on remount and offers a later release after a day", async () => {
    await render()
    await optIn()
    await act(async () => host.querySelector<HTMLButtonElement>("button[aria-label='Dismiss update notice']")!.click())
    expect(host.querySelector("[role=status]")).toBeNull()
    await act(async () => root.render(null))
    await render()
    expect(host.querySelector("[role=status]")).toBeNull()
    expect(fetcher).toHaveBeenCalledTimes(1)
    fetcher.mockImplementation(async () => release("v0.5.0"))
    vi.setSystemTime(Date.now() + updateInterval - 60_000)
    await act(async () => { await vi.advanceTimersByTimeAsync(59_999) })
    expect(fetcher).toHaveBeenCalledTimes(1)
    await act(async () => { await vi.advanceTimersByTimeAsync(1) })
    expect(fetcher).toHaveBeenCalledTimes(2)
    expect(host.querySelector("[role=status]")?.textContent).toContain("v0.5.0")
  })

  it("shows no failure message and makes no retry before a day", async () => {
    fetcher.mockRejectedValue(new TypeError("offline"))
    await render()
    await optIn()
    expect(host.querySelector("[role=status], [role=alert]")).toBeNull()
    expect(host.textContent).not.toContain("offline")
    vi.setSystemTime(Date.now() + updateInterval - 60_001)
    await act(async () => { await vi.advanceTimersByTimeAsync(60_000) })
    expect(fetcher).toHaveBeenCalledTimes(1)
  })

  it("hides a response that arrives after opting out", async () => {
    let finish!: (value: Response) => void
    fetcher.mockImplementation(() => new Promise((resolve) => { finish = resolve }))
    await render()
    await optIn()
    await optIn()
    await act(async () => { finish(release("v0.4.0")) })
    expect(host.querySelector<HTMLInputElement>("input")?.checked).toBe(false)
    expect(host.querySelector("[role=status]")).toBeNull()
  })

  it("reflects consent and dismissal changes from another tab", async () => {
    await render()
    await act(async () => {
      localStorage.setItem("openport.updates.enabled", "true")
      window.dispatchEvent(new StorageEvent("storage", { key: "openport.updates.enabled" }))
    })
    expect(host.querySelector("[role=status]")).not.toBeNull()
    // Navigating away from Status must leave the global notice subscribed.
    await render(false)
    await act(async () => {
      localStorage.setItem("openport.updates.dismissed", "v0.4.0")
      window.dispatchEvent(new StorageEvent("storage", { key: "openport.updates.dismissed" }))
    })
    expect(host.querySelector("[role=status]")).toBeNull()
  })
})
