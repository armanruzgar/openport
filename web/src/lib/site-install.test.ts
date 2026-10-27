// @vitest-environment jsdom
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import html from "../../../site/index.html?raw"
import script from "../../../site/install.js?raw"
import terminalScript from "../../../site/site.js?raw"

beforeEach(() => { document.documentElement.innerHTML = html })
afterEach(() => { vi.clearAllTimers(); vi.useRealTimers(); vi.restoreAllMocks(); vi.unstubAllGlobals() })
function start() { new Function(script)() }

it("keeps Docker available without JavaScript and points Docs at the first ten minutes", () => {
  expect(document.getElementById("install-command")!.textContent).toContain("docker run")
  const docs = [...document.querySelectorAll("a")].find(a => a.textContent?.includes("Docs"))!
  expect(docs.href).toContain("docs/first-ten-minutes.md")
  expect(document.querySelector('a[href="https://github.com/armanruzgar/openport/discussions"]')).not.toBeNull()
})

it("switches the command and explanation with mouse and keyboard using one tab stop", () => {
  start()
  const brew = document.getElementById("install-homebrew")!
  brew.click()
  expect(document.getElementById("install-command")!.textContent).toContain("openportd --print-url --open")
  expect(document.getElementById("install-description")!.textContent).toContain("Intel")
  expect(brew.getAttribute("aria-selected")).toBe("true")
  brew.dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowLeft", bubbles: true }))
  expect(document.getElementById("install-command")!.textContent).toContain("docker run")
  expect(document.activeElement?.id).toBe("install-docker")
  expect(document.querySelectorAll('[role="tab"][tabindex="0"]')).toHaveLength(1)
})

it.each(["", "https://demo.example.com", "http://deployed.openport.dev", "javascript:alert(1)", "https://user:pass@deployed.openport.dev", "https://deployed.openport.dev/#token=secret"])("hides absent, placeholder or unsafe demo URL %s", url => {
  const demo = document.getElementById("hosted-demo")!
  demo.dataset.demoUrl = url
  start()
  expect(demo.hidden).toBe(true)
  expect(demo.hasAttribute("href")).toBe(false)
})

it("reveals the browser demo only after an HTTPS URL is configured", () => {
  const demo = document.getElementById("hosted-demo")! as HTMLAnchorElement
  demo.dataset.demoUrl = "https://deployed.openport.dev"
  start()
  expect(demo.hidden).toBe(false)
  expect(demo.href).toBe("https://deployed.openport.dev/")
})

it("copies the displayed terminal command even after switching install tabs, with a manual fallback", async () => {
  vi.useFakeTimers()
  vi.stubGlobal("matchMedia", () => ({ matches: true, addEventListener: vi.fn() }))
  vi.stubGlobal("IntersectionObserver", class { observe() {} })
  Object.defineProperty(HTMLElement.prototype, "scrollIntoView", { configurable: true, value: vi.fn() })
  const writeText = vi.fn().mockResolvedValue(undefined)
  Object.defineProperty(navigator, "clipboard", { configurable: true, value: { writeText } })
  start()
  new Function(terminalScript)()
  document.getElementById("install-homebrew")!.click()
  document.querySelector<HTMLButtonElement>('[data-cmd="install"]')!.click()
  const result = document.querySelector<HTMLElement>("#out .res")!
  expect(result.textContent).toContain("Needs Homebrew on macOS")
  const command = result.querySelector("code")!.textContent!
  document.getElementById("install-docker")!.click()
  result.querySelector("button")!.click()
  await Promise.resolve()
  expect(writeText).toHaveBeenCalledWith(command)
  writeText.mockRejectedValue(new Error("denied"))
  result.querySelector("button")!.click()
  await Promise.resolve()
  expect(document.getSelection()!.toString()).toBe(command)
  expect(document.getElementById("hosted-demo")!.hidden).toBe(true)
})
