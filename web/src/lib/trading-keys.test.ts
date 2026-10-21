// @vitest-environment jsdom
import { expect, it } from "vitest"
import { chainKeyFocus, isEditing, shortcutAllowed } from "./keyboard"

it.each(["input", "textarea", "select", '[contenteditable="true"]', '[role="combobox"]'])("does not fire inside %s", (selector) => {
  const root = document.createElement("div")
  root.innerHTML = selector.startsWith("[") ? `<div ${selector.slice(1, -1)}><span>edit</span></div>` : `<${selector}></${selector}>`
  const target = root.querySelector("span") ?? root.firstElementChild
  expect(isEditing(target)).toBe(true)
  expect(shortcutAllowed({ target, defaultPrevented: false, isComposing: false, repeat: false, altKey: false, ctrlKey: false, metaKey: false })).toBe(false)
})
it("rejects composition, repeats, consumed and modified keys; allows command-K modifiers explicitly", () => {
  const event = { target: null, defaultPrevented: false, isComposing: false, repeat: false, altKey: false, ctrlKey: false, metaKey: false }
  expect(shortcutAllowed(event)).toBe(true)
  for (const key of ["defaultPrevented", "isComposing", "repeat", "altKey", "ctrlKey", "metaKey"] as const) expect(shortcutAllowed({ ...event, [key]: true })).toBe(false)
  expect(shortcutAllowed({ ...event, metaKey: true }, true)).toBe(true)
})
it("moves between strikes and option sides without wrapping into another contract", () => {
  expect(chainKeyFocus("ArrowDown", 0, "call", 3)).toEqual({ index: 1, side: "call" })
  expect(chainKeyFocus("ArrowUp", 0, "put", 3)).toEqual({ index: 0, side: "put" })
  expect(chainKeyFocus("ArrowRight", 1, "call", 3)).toEqual({ index: 1, side: "put" })
  expect(chainKeyFocus("ArrowLeft", 1, "put", 3)).toEqual({ index: 1, side: "call" })
  expect(chainKeyFocus("End", 0, "put", 3)).toEqual({ index: 2, side: "put" })
  expect(chainKeyFocus("ArrowDown", 0, "call", 0)).toBeNull()
  expect(chainKeyFocus("b", 0, "call", 3)).toBeNull()
})
