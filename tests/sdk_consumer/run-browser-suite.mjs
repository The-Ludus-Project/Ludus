// Runs one suite of installed-SDK browser tests named in browser-suites.json.
//
// Batches run main's workflow definitions against the develop candidate's
// sources, so a test or page named only in the workflow is missing from, or
// stale in, every batch until promotion. The candidate therefore owns the list:
// webgpu-probe.yml uploads each consumer directory by pattern and calls this
// runner with a suite name. See
// docs/development/continuous-integration.md#batch-workflow-definitions.
//
// Usage: node run-browser-suite.mjs <suite> <consumer artifact directory> <results directory>
// <suite> may end in '*' to run every suite whose name starts with the rest, in file
// order ('renderer-*'), so a new suite needs no workflow change.
// Every test runs even after a failure; the exit status is non-zero if any failed.
import {readFile} from 'node:fs/promises';
import {spawnSync} from 'node:child_process';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';

const [suite, artifacts, results] = process.argv.slice(2);
const suites = JSON.parse(await readFile(new URL('./browser-suites.json', import.meta.url), 'utf8'));
const names = typeof suite === 'string' && suite.endsWith('*')
  ? Object.keys(suites).filter(name => name.startsWith(suite.slice(0, -1)))
  : (Object.hasOwn(suites, suite) ? [suite] : []);
const tests = names.flatMap(name => suites[name]);
if (names.length === 0 || tests.length === 0 || artifacts === undefined || results === undefined) {
  console.error(`Usage: run-browser-suite.mjs <suite> <artifacts> <results>; suites: ${Object.keys(suites).join(', ')}`);
  process.exit(2);
}
const failed = [];
for (const test of tests) {
  console.log(`::group::${test.title}`);
  const script = fileURLToPath(new URL(`./${test.script}`, import.meta.url));
  const outcome = spawnSync(process.execPath, [script, artifacts, join(results, test.results)], {stdio: 'inherit'});
  console.log('::endgroup::');
  if (outcome.status !== 0) {
    console.log(`::error::${test.title} (${test.script}) failed with ${outcome.error ?? `status ${outcome.status}`}.`);
    failed.push(test.script);
  }
}
if (failed.length > 0) {
  console.error(`${failed.length} of ${tests.length} ${suite} browser tests failed: ${failed.join(', ')}`);
  process.exit(1);
}
console.log(`All ${tests.length} ${suite} browser tests passed.`);
