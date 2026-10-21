// @vitest-environment jsdom
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { status } from "../test/trading-fixtures"
import { diagnostics } from "../lib/diagnostics"
import { Diagnostics } from "./Diagnostics"

vi.mock("../api/live", async original => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let root: Root
let host: HTMLDivElement
const fixture = {
  ...status, version: "0.4.0", build: { revision: "abc123-dirty", compiler: "Clang-18", platform: "Linux-aarch64" },
  provider: { ...status.provider, name: "demo", simulated: true },
  feed: { ...status.feed, state: "error" as const, message: "HTTP 401 https://user:password@provider/?api_key=secret" },
  series: { enabled: true, directory: "/private/user/series", rows_today: 0, last_write: null, last_error: "/private/user/secret-file" },
  underlyings: [{ ...status.underlyings[0]!, last_error: "JOURNAL_LOCKED: /private/journal token=secret" }],
}
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  vi.mocked(useLive).mockReturnValue(liveState(fixture, null, "open"))
  Object.defineProperty(navigator, "clipboard", { configurable: true, value: { writeText: vi.fn().mockResolvedValue(undefined) } })
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.restoreAllMocks(); vi.unstubAllGlobals() })

it("exports a small allowlist and error categories without secrets, paths, positions or feed timestamps", () => {
  const report = diagnostics(fixture, fixture.feed, "open")
  for (const expected of ["0.4.0", "abc123-dirty", "Linux-aarch64", "demo (simulated)", "HTTP_401", "JOURNAL_LOCKED", "history store: error reported"])
    expect(report).toContain(expected)
  for (const secret of ["secret", "/private", "password", "https://", "SPX", fixture.feed.updated!]) expect(report).not.toContain(secret)
  expect(diagnostics(status, status.feed, "closed")).toContain("Build: unavailable")
})

it("copies only on request and prefills an issue for the user to review and submit", async () => {
  await act(async () => root.render(<Diagnostics />))
  expect(navigator.clipboard.writeText).not.toHaveBeenCalled()
  expect(host.querySelector("a")).toBeNull()
  await act(async () => host.querySelector("button")!.click())
  const report = host.querySelector("textarea")!.value
  expect(navigator.clipboard.writeText).toHaveBeenCalledWith(report)
  expect(host.textContent).toContain("Diagnostics copied")
  const issue = new URL(host.querySelector("a")!.href)
  expect(issue.origin).toBe("https://github.com")
  expect(issue.searchParams.get("template")).toBe("bug_report.yml")
  expect(issue.searchParams.get("diagnostics")).toBe(report)
  expect(issue.searchParams.get("version")).toBe("0.4.0")
})

it("keeps a selectable report when clipboard permission is denied", async () => {
  vi.mocked(navigator.clipboard.writeText).mockRejectedValue(new Error("denied"))
  await act(async () => root.render(<Diagnostics />))
  await act(async () => host.querySelector("button")!.click())
  expect(host.textContent).toContain("Clipboard unavailable")
  expect(host.querySelector("textarea")!.value).toContain("OpenPort: 0.4.0")
})
