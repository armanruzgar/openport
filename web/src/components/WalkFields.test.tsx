// @vitest-environment jsdom
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { useWalk } from "./WalkFields"

let host: HTMLDivElement
let root: Root
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); vi.unstubAllGlobals()
})
function Probe({ start, up, step, cap }: { start: string; up: boolean; step: string; cap: string }) {
  const walk = useWalk(true, start, up, step, cap)
  return <div data-valid={String(walk.valid)} data-walk={JSON.stringify(walk.walk ?? null)}>{walk.fields}</div>
}
const draw = (props: { start: string; up: boolean; step: string; cap: string }) => act(async () => root.render(<Probe {...props} />))
const input = (label: string) => [...host.querySelectorAll("label")].find((l) => l.textContent?.startsWith(label))!.querySelector("input")!
const state = () => host.firstElementChild as HTMLElement
async function type(label: string, value: string) {
  const element = input(label)
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(element, value)
    element.dispatchEvent(new Event("input", { bubbles: true }))
  })
}

it("takes the current side's cap and the current step until the trader types them", async () => {
  // A buy ticket caps at the ask; flipped to sell, at the bid.
  await draw({ start: "15.00", up: true, step: "0.10", cap: "15.50" })
  await draw({ start: "15.40", up: false, step: "0.05", cap: "15.00" })
  await act(async () => host.querySelector<HTMLInputElement>('[aria-label="Walk limit"]')!.click())
  expect(input("Walk cap").value).toBe("15.00")
  expect(input("Walk step").value).toBe("0.05")
  expect(state().dataset.valid).toBe("true")
  expect(JSON.parse(state().dataset.walk!)).toEqual({ step: "0.05", seconds: 10, limit: "15.00" })
  await type("Walk cap", "14.90")
  await draw({ start: "15.40", up: false, step: "0.05", cap: "14.80" })
  expect(input("Walk cap").value).toBe("14.90")
})

it("fills a cap that was missing when the ticket opened once the quote arrives", async () => {
  await draw({ start: "1.00", up: true, step: "0.05", cap: "" })
  await act(async () => host.querySelector<HTMLInputElement>('[aria-label="Walk limit"]')!.click())
  expect(state().dataset.valid).toBe("false")
  await draw({ start: "1.00", up: true, step: "0.10", cap: "1.20" })
  expect(input("Walk cap").value).toBe("1.20")
  expect(input("Walk step").value).toBe("0.10")
  expect(state().dataset.valid).toBe("true")
})
