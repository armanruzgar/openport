/**
 * A random version 4 UUID, for client order IDs and local records. Browsers offer
 * crypto.randomUUID only in a secure context (HTTPS or a loopback address), so a
 * terminal opened over plain HTTP on a LAN address or host name builds one from
 * crypto.getRandomValues, which every context has.
 */
export function randomId(): string {
  if (typeof crypto.randomUUID === "function") return crypto.randomUUID()
  const bytes = crypto.getRandomValues(new Uint8Array(16))
  bytes[6] = (bytes[6]! & 0x0f) | 0x40  // version 4
  bytes[8] = (bytes[8]! & 0x3f) | 0x80  // RFC 9562 variant
  const hex = Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("")
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`
}
