// @vitest-environment jsdom
import { act, type ReactNode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { clearSavedLayout, ErrorBoundary } from "./ErrorBoundary"

let root: Root, host: HTMLDivElement
let fail = true
function Page() {
  if (fail) throw new TypeError("Cannot read properties of undefined (reading 'trim')")
  return <p>Positions page</p>
}
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  // React reports each caught error on the console; the test reads it from the page instead.
  vi.spyOn(console, "error").mockImplementation(() => {})
  fail = true
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); localStorage.clear()
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})
const render = (page: ReactNode) => act(async () => root.render(<><nav>Sidebar · Kill switch</nav>{page}</>))
const click = (label: string) => act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === label)!.click())

it("keeps a page's render error in its place, with the rest of the terminal still there", async () => {
  await render(<ErrorBoundary label="Positions" resetKey="positions"><Page /></ErrorBoundary>)
  expect(host.textContent).toContain("Sidebar · Kill switch")
  expect(host.querySelector('[role="alert"]')?.textContent).toContain("Positions could not be displayed.")
  expect(host.textContent).toContain("reading 'trim'")
  fail = false
  await click("Try again")
  expect(host.textContent).toContain("Positions page")
  // A failure stays until another page (or symbol) is chosen.
  fail = true
  await render(<ErrorBoundary label="Positions" resetKey="positions"><Page /></ErrorBoundary>)
  expect(host.querySelector('[role="alert"]')).not.toBeNull()
  fail = false
  await render(<ErrorBoundary label="Orders" resetKey="orders"><Page /></ErrorBoundary>)
  expect(host.querySelector('[role="alert"]')).toBeNull()
  expect(host.textContent).toContain("Positions page")
})

it("resets this browser's saved drafts and layout, and nothing else", async () => {
  for (const key of ["openport.what-if.live:main", "openport.analyze.SPX", "openport.drawings.SPX", "openport.chain.SPX", "openport.workspaces"])
    localStorage.setItem(key, "{}")
  for (const key of ["openport.alerts", "openport.account", "openport.rule-packs"]) localStorage.setItem(key, "[]")
  await render(<ErrorBoundary label="Positions" resetKey="positions"><Page /></ErrorBoundary>)
  fail = false
  await click("Reset saved layout")
  expect(host.textContent).toContain("Positions page")
  expect(Object.keys(localStorage).sort()).toEqual(["openport.account", "openport.alerts", "openport.rule-packs"])
  expect(clearSavedLayout()).toBe(0)
})
