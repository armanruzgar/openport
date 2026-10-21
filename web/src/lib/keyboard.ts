/** Roving focus for radio groups and expiry tabs. */
export function groupKeyIndex(key: string, index: number, count: number): number | null {
  if (count <= 0) return null
  if (key === "Home") return 0
  if (key === "End") return count - 1
  if (key === "ArrowRight" || key === "ArrowDown") return (index + 1) % count
  if (key === "ArrowLeft" || key === "ArrowUp") return (index - 1 + count) % count
  return null
}

/** Editable controls own their keys, including nested contenteditable content. */
export function isEditing(target: EventTarget | null): boolean {
  return typeof Element !== "undefined" && target instanceof Element && !!target.closest(
    'input, textarea, select, [contenteditable]:not([contenteditable="false"]), [role="textbox"], [role="combobox"], [role="slider"], [role="spinbutton"]',
  )
}
export function shortcutAllowed(event: Pick<KeyboardEvent, "target" | "defaultPrevented" | "isComposing" | "repeat" | "altKey" | "ctrlKey" | "metaKey">, command = false): boolean {
  return !event.defaultPrevented && !event.isComposing && !event.repeat && !event.altKey &&
    (command || (!event.ctrlKey && !event.metaKey)) && !isEditing(event.target)
}

/** Chain arrows stop at the edges; horizontal arrows choose calls or puts. */
export function chainKeyFocus(key: string, index: number, side: "call" | "put", count: number) {
  if (count <= 0) return null
  if (key === "ArrowUp") return { index: Math.max(0, index - 1), side }
  if (key === "ArrowDown") return { index: Math.min(count - 1, index + 1), side }
  if (key === "Home") return { index: 0, side }
  if (key === "End") return { index: count - 1, side }
  if (key === "ArrowLeft") return { index, side: "call" as const }
  if (key === "ArrowRight") return { index, side: "put" as const }
  return null
}

/** Activatable controls (buttons, links, toggles) keep Enter for their own native click. */
const activatable = 'button, a[href], summary, [role="button"], [role="radio"], [role="checkbox"], [role="switch"], [role="tab"], [role="menuitem"], [role="option"]'
/** Enter on a ticket's non-editable, non-activatable content moves focus to its review.
 * Fields keep Enter (it submits the form natively) and buttons keep their own click. */
export function reviewTicketKey(event: import("react").KeyboardEvent<HTMLFormElement>) {
  if (event.key !== "Enter" || !shortcutAllowed(event.nativeEvent) ||
      (event.target instanceof Element && event.target.closest(activatable))) return
  event.preventDefault()
  event.stopPropagation()
  event.currentTarget.querySelector<HTMLElement>("[data-ticket-review]")?.focus()
}
