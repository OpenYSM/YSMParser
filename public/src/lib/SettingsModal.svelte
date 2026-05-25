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
  import { openFolderDialog, setSavedOutputDir } from "./tauri.js";

  interface Props {
    outputDir: string;
    onClose: () => void;
    onSave: (dir: string) => void;
  }
  let { outputDir, onClose, onSave }: Props = $props();

  let draft = $state("");
  let picking = $state(false);
  let error = $state("");
  let firstFocusEl: HTMLButtonElement | undefined = $state();

  $effect(() => {
    draft = outputDir;
  });

  $effect(() => {
    firstFocusEl?.focus();
  });

  async function pickFolder() {
    picking = true;
    error = "";
    try {
      const chosen = await openFolderDialog();
      if (chosen) draft = chosen;
    } catch (err) {
      error = String(err instanceof Error ? err.message : err);
    } finally {
      picking = false;
    }
  }

  async function save() {
    await setSavedOutputDir(draft);
    onSave(draft);
    onClose();
  }

  function onKey(e: KeyboardEvent) {
    if (e.key === "Escape") onClose();
  }

  function onOverlayClick(e: MouseEvent) {
    if (e.target === e.currentTarget) onClose();
  }
</script>

<svelte:window onkeydown={onKey} />

<!-- svelte-ignore a11y_click_events_have_key_events -->
<!-- svelte-ignore a11y_no_static_element_interactions -->
<div
  class="overlay"
  onclick={onOverlayClick}
>
  <div
    class="modal"
    role="dialog"
    aria-modal="true"
    aria-labelledby="settings-title"
  >
    <div class="modal-header">
      <h2 id="settings-title">Settings</h2>
      <button class="close-btn" onclick={onClose} aria-label="Close" bind:this={firstFocusEl}>✕</button>
    </div>

    <div class="modal-body">
      <label class="field-label" for="output-dir">Output folder</label>
      <p class="field-hint">Decrypted files will be saved here. Required to run the parser.</p>
      <div class="dir-row">
        <input
          id="output-dir"
          class="dir-input"
          type="text"
          bind:value={draft}
          placeholder="Choose a folder…"
          readonly
        />
        <button class="btn btn-secondary" onclick={pickFolder} disabled={picking}>
          {picking ? "…" : "Browse"}
        </button>
      </div>
      {#if error}
        <p class="field-error" role="alert">{error}</p>
      {/if}
    </div>

    <div class="modal-footer">
      <button class="btn btn-ghost" onclick={onClose}>Cancel</button>
      <button class="btn btn-primary" onclick={save} disabled={!draft}>Save</button>
    </div>
  </div>
</div>

<style>
  .overlay {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.7);
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 1rem;
    z-index: 100;
  }
  .modal {
    background: var(--surface-1);
    border: 1px solid var(--border);
    border-radius: 12px;
    width: min(480px, 100%);
    max-height: calc(100dvh - 2rem);
    overflow-y: auto;
  }
  .modal-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 1.25rem 1.5rem 0;
  }
  h2 {
    font-size: 1.1rem;
    font-weight: 600;
    color: var(--text-primary);
    margin: 0;
  }
  .close-btn {
    background: none;
    border: none;
    color: var(--text-muted);
    cursor: pointer;
    font-size: 1rem;
    padding: 0.25rem 0.5rem;
    border-radius: 6px;
  }
  .close-btn:hover {
    color: var(--text-primary);
    background: var(--surface-2);
  }
  .modal-body {
    padding: 1.25rem 1.5rem;
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .field-label {
    font-size: 0.85rem;
    font-weight: 600;
    color: var(--text-primary);
  }
  .field-hint {
    font-size: 0.78rem;
    color: var(--text-muted);
    margin: 0;
  }
  .field-error {
    font-size: 0.78rem;
    color: var(--danger);
    margin: 0;
    overflow-wrap: anywhere;
  }
  .dir-row {
    display: flex;
    gap: 0.5rem;
    margin-top: 0.25rem;
  }
  .dir-input {
    flex: 1;
    background: var(--surface-2);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 0.5rem 0.75rem;
    color: var(--text-primary);
    font-size: 0.85rem;
    font-family: var(--font-mono);
    min-width: 0;
    cursor: default;
  }
  .modal-footer {
    display: flex;
    justify-content: flex-end;
    gap: 0.5rem;
    padding: 0 1.5rem 1.25rem;
  }
  .btn {
    padding: 0.5rem 1.1rem;
    border-radius: 8px;
    font-size: 0.85rem;
    font-weight: 500;
    cursor: pointer;
    border: none;
    transition: background 0.12s, opacity 0.12s;
  }
  .btn:disabled {
    opacity: 0.45;
    cursor: not-allowed;
  }
  .btn-primary {
    background: var(--accent);
    color: #000;
  }
  .btn-primary:hover:not(:disabled) {
    background: color-mix(in srgb, var(--accent) 80%, #fff);
  }
  .btn-secondary {
    background: var(--surface-2);
    color: var(--text-primary);
    border: 1px solid var(--border);
  }
  .btn-ghost {
    background: none;
    color: var(--text-muted);
    border: 1px solid transparent;
  }
  .btn-ghost:hover {
    color: var(--text-primary);
    border-color: var(--border);
  }
</style>
