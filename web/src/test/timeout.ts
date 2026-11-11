// Every test and hook must finish within this. It only stops a test that hangs:
// a worker on a loaded machine can go minutes without running, so it is generous,
// yet it ends well inside the CI web job's 15 minutes, so a hung test is reported
// as a failure instead of the job being cancelled. Waits for a render have their
// own, shorter deadline (renderWait in ./render).
export const testTimeout = 5 * 60_000
