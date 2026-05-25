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
  import { formatSize } from "./format.js";

  const ROW_HEIGHT = 46;
  const OVERSCAN = 6;

  interface QueueItem {
    name: string;
    size: number;
  }

  interface Props {
    items: QueueItem[];
    onRemove?: (index: number) => void;
  }
  let { items, onRemove }: Props = $props();

  let viewport = $state<HTMLDivElement | null>(null);
  let scrollTop = $state(0);
  let viewportHeight = $state(0);

  let rowCount = $derived(items.length);
  let totalHeight = $derived(rowCount * ROW_HEIGHT);
  let visibleCount = $derived(Math.max(1, Math.ceil(viewportHeight / ROW_HEIGHT)));
  let startIndex = $derived(Math.max(0, Math.floor(scrollTop / ROW_HEIGHT) - OVERSCAN));
  let endIndex = $derived(Math.min(rowCount, startIndex + visibleCount + OVERSCAN * 2));
  let visibleItems = $derived(items.slice(startIndex, endIndex));
  let indexDigits = $derived(Math.max(2, String(Math.max(1, rowCount)).length));
  let summary = $derived(
    rowCount === 0
      ? "No files selected"
      : `Showing ${startIndex + 1} to ${endIndex} of ${rowCount} selected files`
  );

  function syncViewportSize() {
    if (!viewport) return;
    viewportHeight = viewport.clientHeight;
  }

  function onScroll(e: Event) {
    scrollTop = (e.currentTarget as HTMLDivElement).scrollTop;
  }

  onMount(() => {
    syncViewportSize();
    const observer = new ResizeObserver(syncViewportSize);
    if (viewport) observer.observe(viewport);
    return () => observer.disconnect();
  });

  $effect(() => {
    if (!viewport) return;
    const maxScrollTop = Math.max(0, totalHeight - viewportHeight);
    if (scrollTop > maxScrollTop) {
      viewport.scrollTop = maxScrollTop;
      scrollTop = maxScrollTop;
    }
  });
</script>

{#if items.length === 0}
  <div class="empty-state">
    <span>No files selected</span>
  </div>
{:else}
  <p id="file-queue-summary" class="sr-only">{summary}</p>
  <!-- svelte-ignore a11y_no_noninteractive_tabindex: scrollable file list must be keyboard-focusable -->
  <div
    class="file-queue-viewport"
    bind:this={viewport}
    onscroll={onScroll}
    tabindex="0"
    role="region"
    aria-label="Selected files"
    aria-describedby="file-queue-summary"
    style={`--index-width: ${indexDigits}ch;`}
  >
    <ul
      class="file-queue"
      role="list"
      aria-label="Selected files"
      style={`height: ${totalHeight}px;`}
    >
      {#each visibleItems as item, i (item.name)}
        {@const absoluteIndex = startIndex + i}
        <li
          class="file-item"
          role="listitem"
          aria-setsize={rowCount}
          aria-posinset={absoluteIndex + 1}
          style={`transform: translateY(${absoluteIndex * ROW_HEIGHT}px);`}
        >
          <span class="file-index" aria-hidden="true">{String(absoluteIndex + 1).padStart(2, "0")}</span>
          <div class="file-info">
            <strong class="file-name">{item.name}</strong>
            <span class="file-size">{formatSize(item.size)}</span>
          </div>
          {#if onRemove}
            <button
              class="remove-btn"
              onclick={() => onRemove(absoluteIndex)}
              aria-label="Remove {item.name}"
              title="Remove"
            >✕</button>
          {/if}
        </li>
      {/each}
    </ul>
  </div>
{/if}

<style>
  .sr-only {
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
  .file-queue-viewport {
    height: 100%;
    max-height: 100%;
    min-height: 0;
    overflow-y: auto;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: var(--surface-2);
  }
  .file-queue {
    position: relative;
    list-style: none;
    padding: 0;
    margin: 0;
    min-height: 100%;
  }
  .empty-state {
    display: flex;
    align-items: center;
    justify-content: center;
    height: 100%;
    min-height: 0;
    padding: 1rem;
    color: var(--text-muted);
    font-size: 0.85rem;
    border: 1px dashed var(--border);
    border-radius: 6px;
  }
  .file-item {
    position: absolute;
    top: 0;
    left: 0;
    right: 0;
    display: flex;
    align-items: center;
    height: 46px;
    gap: 0.55rem;
    padding: 0.42rem 0.6rem;
    border-bottom: 1px solid var(--border);
    contain: layout paint;
  }
  .file-index {
    font-family: var(--font-mono);
    font-size: 0.7rem;
    color: var(--text-muted);
    width: calc(var(--index-width) + 0.8rem);
    flex: 0 0 auto;
    text-align: right;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .file-info {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 0.1rem;
  }
  .file-name {
    font-size: 0.85rem;
    color: var(--text-primary);
    font-weight: 500;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .file-size {
    font-size: 0.72rem;
    color: var(--text-muted);
    font-family: var(--font-mono);
  }
  .remove-btn {
    background: none;
    border: 1px solid transparent;
    color: var(--text-muted);
    cursor: pointer;
    font-size: 0.75rem;
    width: 1.65rem;
    height: 1.65rem;
    border-radius: 4px;
    line-height: 1;
    transition: color 0.12s, background 0.12s;
    flex: 0 0 auto;
  }
  .remove-btn:hover {
    color: var(--danger);
    background: color-mix(in srgb, var(--danger) 12%, transparent);
  }

  @media (max-width: 768px) {
    .file-item {
      padding-block: 0.35rem;
    }
  }
</style>
