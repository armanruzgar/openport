import { api as rawApi } from "../api/client"
import { captureDestination, withDestination } from "../api/destination"

/** Routing contract tests explicitly start a fresh action for each invocation.
 * UI tests use the actual components and their opening destination instead.
 */
export const api = new Proxy(rawApi, {
  get(target, key: keyof typeof rawApi) {
    return (...args: unknown[]) => withDestination(captureDestination(), () =>
      (target[key] as (...args: unknown[]) => unknown)(...args))
  },
})
