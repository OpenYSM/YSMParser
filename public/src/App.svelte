<!-- The frontend of YSMParser.
  - Copyright (C) 2026 MiRinChan
  - This program is free software; you can redistribute it and/or modify
  - it under the terms of the GNU General Public License as published by
  - the Free Software Foundation; either version 2 of the License, or
  - (at your option) any later version.
  -
  - This program is distributed in the hope that it will be useful,
  - but WITHOUT ANY WARRANTY; without even the implied warranty of
  - MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
  - GNU General Public License for more details.
  -
  - You should have received a copy of the GNU General Public License along
  - with this program; if not, see < https://www.gnu.org/licenses/>.
  -->

<script lang="ts">
  import { onMount } from "svelte";
  import DropZone from "./lib/DropZone.svelte";
  import FileQueue from "./lib/FileQueue.svelte";
  import ProgressBar from "./lib/ProgressBar.svelte";
  import LogConsole from "./lib/LogConsole.svelte";
  import SettingsModal from "./lib/SettingsModal.svelte";
  import { formatSize } from "./lib/format.js";
  import {
    isTauri,
    runParserNative,
    getSavedOutputDir,
    openPathInFileBrowser,
    statInputFiles,
    prepareInputDirFromPaths,
  } from "./lib/tauri.js";
  import { type WasmModule, initWasm, runWasm } from "./lib/wasm.js";
  import JSZip from "jszip";

  const WASM_BATCH_BYTES = 96 * 1024 * 1024;

  interface QueueItem {
    name: string;
    size: number;
    file?: File;
    path?: string;
  }

  // ── state ──────────────────────────────────────────────────────────────────
  let items = $state<QueueItem[]>([]);
  let logs = $state<string[]>([]);
  let progress = $state(0);
  let progressLabel = $state("Idle");
  let running = $state(false);
  let outputFileCount = $state(0);
  let outputBytes = $state(0);
  let outputZip = $state<Blob | null>(null);
  let outputDir = $state("");
  let showSettings = $state(false);
  let wasmMod = $state<WasmModule | null>(null);
  let wasmReady = $state(false);
  let wasmError = $state("");

  // ── derived ────────────────────────────────────────────────────────────────
  let totalSize = $derived(items.reduce((s, it) => s + it.size, 0));
  let canRun = $derived(
    items.length > 0 && !running && (isTauri || wasmReady)
  );

  // ── helpers ────────────────────────────────────────────────────────────────
  function log(line: string) {
    logs = [...logs, line];
  }

  // ── init ───────────────────────────────────────────────────────────────────
  onMount(async () => {
    if (isTauri) {
      const saved = await getSavedOutputDir();
      if (saved) {
        outputDir = saved;
      } else {
        showSettings = true;
      }
      return;
    }
    try {
      wasmMod = await initWasm(log);
      wasmReady = true;
      log("WASM runtime ready.");
    } catch (err) {
      wasmError = err instanceof Error ? err.message : String(err);
      log(`Runtime error: ${wasmError}`);
    }
  });

  function setProgress(pct: number, label: string) {
    progress = pct;
    progressLabel = label;
  }

  function formatMs(ms: number) {
    return ms >= 1000 ? `${(ms / 1000).toFixed(2)}s` : `${ms.toFixed(0)}ms`;
  }

  function appendItems(newItems: QueueItem[]) {
    const names = new Set(items.map((it) => it.name));
    const deduped = newItems.filter((it) => !names.has(it.name));
    items = [...items, ...deduped];
    outputZip = null;
    outputFileCount = 0;
    outputBytes = 0;
  }

  function addFiles(files: File[]) {
    appendItems(files.map((f) => ({ name: f.name, size: f.size, file: f })));
  }

  async function addPaths(paths: string[]) {
    try {
      const stats = await statInputFiles(paths);
      appendItems(
        stats.map((s) => ({ name: s.name, size: s.size, path: s.path }))
      );
    } catch (err) {
      log(`Error reading file metadata: ${err instanceof Error ? err.message : String(err)}`);
    }
  }

  function removeItem(i: number) {
    items = items.filter((_, idx) => idx !== i);
  }

  // ── run parser ─────────────────────────────────────────────────────────────
  async function run() {
    if (!canRun) return;
    if (isTauri && !outputDir) {
      showSettings = true;
      return;
    }
    running = true;
    logs = [];
    outputFileCount = 0;
    outputBytes = 0;
    outputZip = null;
    setProgress(2, "Starting…");

    try {
      if (isTauri) {
        await runTauri();
      } else {
        await runWasmMode();
      }
    } catch (err) {
      log(`Error: ${err instanceof Error ? err.message : String(err)}`);
      setProgress(0, "Failed");
    } finally {
      running = false;
    }
  }

  async function runTauri() {
    setProgress(10, "Preparing files");
    const paths = items.map((it) => it.path).filter((p): p is string => !!p);
    log(`Linking ${paths.length} input file(s) into temp dir…`);

    const inputDir = await prepareInputDirFromPaths(paths);
    setProgress(25, "Running parser");
    log(`Input temp dir: ${inputDir}`);
    log(`Output dir: ${outputDir}`);

    const stdout = await runParserNative(inputDir, outputDir);
    for (const line of stdout.split("\n")) {
      if (line) log(line);
    }

    setProgress(100, "Done");
    log(`Done. Files saved to: ${outputDir}`);
  }

  async function runWasmMode() {
    if (!wasmMod) throw new Error("WASM runtime not ready");
    const files = items.map((it) => it.file).filter((f): f is File => !!f);
    const estimatedBatches = Math.max(
      1,
      Math.ceil(totalSize / WASM_BATCH_BYTES)
    );
    log(
      `Preparing ${files.length} file(s) in ${estimatedBatches} batch(es), up to ${formatSize(WASM_BATCH_BYTES)} per batch.`
    );

    const zip = new JSZip();
    const result = await runWasm(wasmMod, files, {
      maxBatchBytes: WASM_BATCH_BYTES,
      onProgress: (pct, label) => setProgress(pct, label),
      onOutputFile: (file) => zip.file(file.path, file.data),
    });
    setProgress(76, "Packaging");
    log(
      `Parser finished. ${result.outputCount} output file(s) from ${result.batchCount} batch(es).`
    );
    log(
      `Timing: input ${formatMs(result.timings.inputWriteMs)}, parser ${formatMs(result.timings.parserMs)}, collect ${formatMs(result.timings.outputCollectMs)}, cleanup ${formatMs(result.timings.cleanupMs)}.`
    );
    for (const batch of result.timings.batches) {
      log(
        `Batch ${batch.batch}: ${batch.fileCount} file(s), ${formatSize(batch.inputBytes)}, parser ${formatMs(batch.parserMs)}, total ${formatMs(batch.totalMs)}.`
      );
    }

    const packageStart = performance.now();
    outputZip = await zip.generateAsync(
      {
        type: "blob",
        compression: "DEFLATE",
        compressionOptions: { level: 6 },
        streamFiles: true,
      },
      (metadata) => {
        setProgress(
          76 + metadata.percent * 0.24,
          `Packaging ${metadata.percent.toFixed(0)}%`
        );
      }
    );
    const packageMs = performance.now() - packageStart;

    outputFileCount = result.outputCount;
    outputBytes = result.outputBytes;
    setProgress(100, `${result.outputCount} file(s) ready`);
    log(
      `ZIP ready — ${result.outputCount} file(s), ${formatSize(result.outputBytes)} unpacked.`
    );
    log(`Timing: packaging ${formatMs(packageMs)}.`);
  }

  // ── download ───────────────────────────────────────────────────────────────
  function download() {
    if (!outputZip) return;
    const url = URL.createObjectURL(outputZip);
    const a = document.createElement("a");
    a.href = url;
    a.download = "YSMParser-output.zip";
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
  }

  async function openOutputFolder() {
    if (outputDir) await openPathInFileBrowser(outputDir);
  }

  function clear() {
    items = [];
    logs = [];
    outputFileCount = 0;
    outputBytes = 0;
    outputZip = null;
    setProgress(0, "Idle");
  }
</script>

<!-- ── Settings modal ────────────────────────────────────────────────── -->
{#if showSettings}
  <SettingsModal
    outputDir={outputDir}
    onClose={() => (showSettings = false)}
    onSave={(dir) => (outputDir = dir)}
  />
{/if}

<!-- ── Main layout ───────────────────────────────────────────────────── -->
<div class="shell">
  <!-- Header -->
  <header class="header">
    <div class="brand">
      <svg class="brand-icon" viewBox="0 0 28 28" fill="none" aria-hidden="true">
        <rect width="28" height="28" rx="7" fill="var(--accent)" />
        <path d="M7 20L14 8l7 12H7z" fill="#000" />
      </svg>
      <span class="brand-name">YSMParser</span>
    </div>
    <div class="header-right">
      {#if isTauri}
        <span class="dir-badge" title={outputDir || "No output folder set"}>
          {outputDir ? outputDir.split(/[\\/]/).pop() : "No folder"}
        </span>
        <button
          class="icon-btn"
          onclick={() => (showSettings = true)}
          title="Settings"
          aria-label="Open settings"
        >
          <svg viewBox="0 0 20 20" fill="currentColor" width="16" height="16" aria-hidden="true">
            <path fill-rule="evenodd" clip-rule="evenodd"
              d="M11.49 3.17c-.38-1.56-2.6-1.56-2.98 0a1.532 1.532 0 01-2.286.948c-1.372-.836-2.942.734-2.106 2.106.54.886.061 2.042-.947 2.287-1.561.379-1.561 2.6 0 2.978a1.532 1.532 0 01.947 2.287c-.836 1.372.734 2.942 2.106 2.106a1.532 1.532 0 012.287.947c.379 1.561 2.6 1.561 2.978 0a1.533 1.533 0 012.287-.947c1.372.836 2.942-.734 2.106-2.106a1.533 1.533 0 01.947-2.287c1.561-.379 1.561-2.6 0-2.978a1.532 1.532 0 01-.947-2.287c.836-1.372-.734-2.942-2.106-2.106a1.532 1.532 0 01-2.287-.947zM10 13a3 3 0 100-6 3 3 0 000 6z" />
          </svg>
        </button>
      {:else}
        <span class="runtime-badge" class:ready={wasmReady} class:error={!!wasmError}>
          {wasmReady ? "WASM Ready" : wasmError ? "Runtime Error" : "Loading…"}
        </span>
      {/if}
    </div>
  </header>

  <!-- Body -->
  <main class="main">
    <!-- Left panel: files -->
    <section class="panel panel-files">
      <div class="panel-head">
        <h2 class="panel-title">Input Files</h2>
        <span class="panel-meta">
          {items.length} file{items.length !== 1 ? "s" : ""}
          {items.length > 0 ? `· ${formatSize(totalSize)}` : ""}
        </span>
      </div>

      <DropZone onFiles={addFiles} onPaths={addPaths} disabled={running} />

      <div class="queue-wrap">
        <FileQueue {items} onRemove={running ? undefined : removeItem} />
      </div>

      <!-- Actions -->
      <div class="actions">
        <button
          class="btn btn-primary"
          onclick={run}
          disabled={!canRun}
        >
          {running ? "Running…" : "Run Parser"}
        </button>

        {#if !isTauri && outputZip}
          <button class="btn btn-secondary" onclick={download}>
            Download ZIP
          </button>
        {/if}

        {#if isTauri && outputDir && progress === 100}
          <button class="btn btn-secondary" onclick={openOutputFolder}>
            Open Folder
          </button>
        {/if}

        {#if items.length > 0 || logs.length > 0}
          <button class="btn btn-ghost" onclick={clear} disabled={running}>
            Clear
          </button>
        {/if}
      </div>
    </section>

    <!-- Right panel: progress + log -->
    <section class="panel panel-log">
      <div class="panel-head">
        <h2 class="panel-title">Output</h2>
        {#if outputFileCount > 0}
          <span class="panel-meta">{outputFileCount} file(s) · {formatSize(outputBytes)}</span>
        {/if}
      </div>

      <ProgressBar percent={progress} label={progressLabel} />

      <LogConsole lines={logs} />

      {#if isTauri && outputDir}
        <div class="output-dir-note">
          <svg viewBox="0 0 16 16" fill="currentColor" width="12" height="12" aria-hidden="true">
            <path d="M1 3.5A1.5 1.5 0 012.5 2h2.764c.958 0 1.76.56 2.311 1.184C7.985 3.648 8.48 4 9 4h4.5A1.5 1.5 0 0115 5.5v7a1.5 1.5 0 01-1.5 1.5h-11A1.5 1.5 0 011 12.5v-9z" />
          </svg>
          <code>{outputDir}</code>
        </div>
      {/if}
    </section>
  </main>

  <footer class="footer">
    <span>YSMParser · <a href="https://github.com/OpenYSM/YSMParser" target="_blank" rel="noopener noreferrer">GitHub</a></span>
  </footer>
</div>

<style>
  .shell {
    display: flex;
    flex-direction: column;
    height: 100dvh;
    min-height: 0;
    overflow: hidden;
  }

  /* ── Header ── */
  .header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
    padding: 0.55rem 1rem;
    border-bottom: 1px solid var(--border);
    background: var(--surface-1);
    flex-shrink: 0;
    flex-wrap: wrap;
  }
  .brand {
    display: flex;
    align-items: center;
    gap: 0.6rem;
  }
  .brand-icon {
    width: 28px;
    height: 28px;
    flex-shrink: 0;
  }
  .brand-name {
    font-weight: 700;
    font-size: 1rem;
    letter-spacing: -0.01em;
    color: var(--text-primary);
  }
  .header-right {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }
  .runtime-badge {
    font-size: 0.72rem;
    font-family: var(--font-mono);
    padding: 0.2rem 0.6rem;
    border-radius: 6px;
    background: var(--surface-3);
    color: var(--text-muted);
    border: 1px solid var(--border);
  }
  .runtime-badge.ready {
    color: var(--success);
    border-color: color-mix(in srgb, var(--success) 30%, transparent);
  }
  .runtime-badge.error {
    color: var(--danger);
    border-color: color-mix(in srgb, var(--danger) 30%, transparent);
  }
  .dir-badge {
    font-size: 0.72rem;
    font-family: var(--font-mono);
    padding: 0.2rem 0.6rem;
    border-radius: 6px;
    background: var(--surface-3);
    color: var(--text-secondary);
    border: 1px solid var(--border);
    max-width: 14rem;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .icon-btn {
    background: none;
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 0.35rem 0.45rem;
    color: var(--text-muted);
    cursor: pointer;
    display: flex;
    align-items: center;
    transition: color 0.12s, border-color 0.12s;
  }
  .icon-btn:hover {
    color: var(--text-primary);
    border-color: var(--text-muted);
  }

  /* ── Main ── */
  .main {
    flex: 1;
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 0.85rem;
    padding: 0.85rem 1rem;
    min-height: 0;
    overflow: hidden;
  }
  @media (max-width: 768px) {
    .main {
      grid-template-columns: 1fr;
      grid-template-rows: minmax(0, 1.1fr) minmax(0, 0.9fr);
      padding: 1rem;
    }
  }

  /* ── Panels ── */
  .panel {
    display: flex;
    flex-direction: column;
    gap: 0.65rem;
    background: var(--surface-1);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 0.85rem 0.95rem;
    min-width: 0;
    min-height: 0;
    overflow: hidden;
  }
  .panel-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
    min-width: 0;
  }
  .panel-title {
    font-size: 0.78rem;
    font-weight: 600;
    color: var(--text-secondary);
    text-transform: uppercase;
    letter-spacing: 0.05em;
  }
  .panel-meta {
    font-size: 0.75rem;
    color: var(--text-muted);
    font-family: var(--font-mono);
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .queue-wrap {
    flex: 1;
    min-height: 0;
    overflow: hidden;
  }

  /* ── Actions ── */
  .actions {
    display: flex;
    gap: 0.5rem;
    flex-wrap: wrap;
    padding-top: 0.1rem;
  }
  .btn {
    padding: 0.45rem 0.85rem;
    border-radius: 6px;
    font-size: 0.83rem;
    font-weight: 500;
    cursor: pointer;
    border: none;
    transition: background 0.12s, opacity 0.12s;
    white-space: nowrap;
  }
  .btn:disabled {
    opacity: 0.4;
    cursor: not-allowed;
  }
  .btn-primary {
    background: var(--accent);
    color: #000;
  }
  .btn-primary:hover:not(:disabled) {
    background: color-mix(in srgb, var(--accent) 82%, #fff);
  }
  .btn-secondary {
    background: var(--surface-3);
    color: var(--text-primary);
    border: 1px solid var(--border);
  }
  .btn-secondary:hover:not(:disabled) {
    border-color: var(--text-muted);
  }
  .btn-ghost {
    background: none;
    color: var(--text-muted);
    border: 1px solid transparent;
  }
  .btn-ghost:hover:not(:disabled) {
    color: var(--text-primary);
    border-color: var(--border);
  }

  /* ── Output dir note ── */
  .output-dir-note {
    display: flex;
    align-items: center;
    gap: 0.45rem;
    color: var(--text-muted);
    font-size: 0.75rem;
    padding: 0.4rem 0.65rem;
    background: var(--surface-2);
    border-radius: 6px;
    border: 1px solid var(--border);
    min-width: 0;
  }
  .output-dir-note code {
    flex: 1;
    min-width: 0;
    font-family: var(--font-mono);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  /* ── Footer ── */
  .footer {
    padding: 0.6rem 1.5rem;
    border-top: 1px solid var(--border);
    font-size: 0.72rem;
    color: var(--text-muted);
    flex-shrink: 0;
  }
</style>
