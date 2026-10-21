import { defineConfig, mergeConfig } from "vitest/config"
import viteConfig from "./vite.config"
import { testTimeout } from "./src/test/timeout"

export default mergeConfig(viteConfig, defineConfig({
  // Tests also inspect the sibling site, OpenAPI document and shared fixtures.
  server: { fs: { allow: [".."] } },
  test: { testTimeout, hookTimeout: testTimeout },
}))
