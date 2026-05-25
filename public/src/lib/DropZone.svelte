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
  import {
    isTauri,
    listenTauriDragDrop,
    openInputFilesDialog,
    preloadInputFilesDialog,
  } from "./tauri.js";

  interface Props {
    onFiles?: (files: File[]) => void;
    onPaths?: (paths: string[]) => void;
    disabled?: boolean;
  }
  let { onFiles, onPaths, disabled = false }: Props = $props();

  let active = $state(false);
  let picking = $state(false);
  let unavailable = $derived(disabled || picking);

  function isYsm(name: string): boolean {
    return name.toLowerCase().endsWith(".ysm");
  }

  function sortedYsmFiles(files: File[]): File[] {
    return files
      .filter((f) => isYsm(f.name))
      .sort((a, b) => a.name.localeCompare(b.name, "zh-CN"));
  }

  function sortedYsmPaths(paths: string[]): string[] {
    return paths.filter(isYsm).sort((a, b) => a.localeCompare(b, "zh-CN"));
  }

  function emitPaths(paths: string[]) {
    const ysm = sortedYsmPaths(paths);
    if (ysm.length) onPaths?.(ysm);
  }

  // ── Tauri mode: native drag-drop + native dialog ───────────────────────
  onMount(() => {
    if (!isTauri) return;
    let unlisten: (() => void) | null = null;
    preloadInputFilesDialog();
    listenTauriDragDrop((phase, paths) => {
      if (unavailable) return;
      switch (phase) {
        case "enter":
        case "over":
          active = true;
          break;
        case "leave":
          active = false;
          break;
        case "drop":
          active = false;
          emitPaths(paths);
          break;
      }
    }).then((u) => {
      unlisten = u;
    });
    return () => unlisten?.();
  });

  async function pickViaTauri() {
    if (unavailable) return;
    picking = true;
    try {
      emitPaths(await openInputFilesDialog());
    } finally {
      picking = false;
    }
  }

  // ── Browser/WASM mode: HTML5 drop + file input ─────────────────────────
  function onDragOver(e: DragEvent) {
    e.preventDefault();
    if (!unavailable) active = true;
  }
  function onDragLeave(e: DragEvent) {
    e.preventDefault();
    active = false;
  }
  function onDrop(e: DragEvent) {
    e.preventDefault();
    active = false;
    if (unavailable || !e.dataTransfer) return;
    onFiles?.(sortedYsmFiles(Array.from(e.dataTransfer.files)));
  }

  function onInput(e: Event) {
    const input = e.currentTarget as HTMLInputElement;
    if (input.files) {
      onFiles?.(sortedYsmFiles(Array.from(input.files)));
      input.value = "";
    }
  }
</script>

{#snippet inner()}
  <div class="dropzone-inner">
    <svg class="dropzone-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true">
      <path stroke-linecap="round" stroke-linejoin="round"
        d="M3 16.5v2.25A2.25 2.25 0 005.25 21h13.5A2.25 2.25 0 0021 18.75V16.5m-13.5-9L12 3m0 0l4.5 4.5M12 3v13.5" />
    </svg>
    <p class="dropzone-primary">Drop <code>.ysm</code> files here</p>
    <p class="dropzone-secondary">or click to browse</p>
  </div>
{/snippet}

{#if isTauri}
  <button
    type="button"
    class="dropzone"
    class:active
    class:disabled={unavailable}
    disabled={unavailable}
    aria-busy={picking}
    aria-label="Choose .ysm files"
    onclick={pickViaTauri}
  >
    {@render inner()}
  </button>
{:else}
  <label
    class="dropzone"
    class:active
    class:disabled={unavailable}
    ondragover={onDragOver}
    ondragleave={onDragLeave}
    ondrop={onDrop}
  >
    <input
      type="file"
      accept=".ysm"
      multiple
      class="dropzone-input"
      aria-label="Choose .ysm files"
      disabled={unavailable}
      oninput={onInput}
    />
    {@render inner()}
  </label>
{/if}

<style>
  .dropzone {
    position: relative;
    display: flex;
    align-items: center;
    justify-content: center;
    width: 100%;
    border: 2px dashed var(--border);
    border-radius: 8px;
    padding: 1.35rem 1rem;
    cursor: pointer;
    transition: border-color 0.15s, background 0.15s;
    text-align: center;
    background: transparent;
    color: inherit;
    font: inherit;
  }
  .dropzone:hover:not(.disabled),
  .dropzone.active,
  .dropzone:focus-within:not(.disabled),
  .dropzone:focus-visible:not(.disabled) {
    border-color: var(--accent);
    background: var(--accent-dim);
  }
  .dropzone.disabled {
    opacity: 0.45;
    cursor: not-allowed;
  }
  .dropzone-input {
    position: absolute;
    width: 1px;
    height: 1px;
    padding: 0;
    margin: -1px;
    overflow: hidden;
    clip: rect(0, 0, 0, 0);
    white-space: nowrap;
    border: 0;
  }
  .dropzone-inner {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 0.28rem;
    pointer-events: none;
  }
  .dropzone-icon {
    width: 2rem;
    height: 2rem;
    color: var(--accent);
    margin-bottom: 0.15rem;
  }
  .dropzone-primary {
    font-size: 0.95rem;
    font-weight: 600;
    color: var(--text-primary);
    margin: 0;
  }
  .dropzone-secondary {
    font-size: 0.8rem;
    color: var(--text-muted);
    margin: 0;
  }
  code {
    background: var(--surface-2);
    border-radius: 4px;
    padding: 1px 5px;
    font-family: var(--font-mono);
    font-size: 0.85em;
  }

  @media (max-width: 768px) {
    .dropzone {
      padding: 0.8rem 0.85rem;
    }
    .dropzone-inner {
      gap: 0.18rem;
    }
    .dropzone-icon {
      width: 1.45rem;
      height: 1.45rem;
      margin-bottom: 0;
    }
    .dropzone-primary {
      font-size: 0.86rem;
    }
    .dropzone-secondary {
      font-size: 0.74rem;
    }
  }
</style>
