import { expect, it } from "vitest"
import { rowWindow } from "./row-window"

it("keeps a large chain bounded while spacer heights preserve every row", () => {
  const window = rowWindow(10000, 320000, 448, 64)
  expect(window.end - window.start).toBe(17)
  expect(window.before + window.after + (window.end - window.start) * 64).toBe(640000)
  expect(window.start).toBe(4995)
})
it("clamps empty, short, negative and past-end windows", () => {
  expect(rowWindow(0, 0, 448, 64)).toEqual({ start: 0, end: 0, before: 0, after: 0 })
  expect(rowWindow(3, -100, 448, 64)).toEqual({ start: 0, end: 3, before: 0, after: 0 })
  expect(rowWindow(10, 100000, 448, 64)).toEqual({ start: 10, end: 10, before: 640, after: 0 })
  const window = rowWindow(100, 6390, 448, 64)
  expect(window.end).toBe(100)
  expect(window.start).toBe(94)
})
