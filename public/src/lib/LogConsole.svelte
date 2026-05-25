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
  import { tick } from "svelte";

  interface Props {
    lines: string[];
  }
  let { lines }: Props = $props();

  let el: HTMLElement;

  $effect(() => {
    lines;
    tick().then(() => {
      if (el) el.scrollTop = el.scrollHeight;
    });
  });
</script>

<div
  class="console"
  bind:this={el}
  role="log"
  aria-live="polite"
  aria-relevant="additions"
  aria-label="Activity log"
>
  {#if lines.length === 0}
    <span class="console-placeholder">Logs will appear here…</span>
  {:else}
    {#each lines as line}
      <div class="console-line">{line}</div>
    {/each}
  {/if}
</div>

<style>
  .console {
    background: var(--surface-2);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 0.75rem 1rem;
    font-family: var(--font-mono);
    font-size: 0.78rem;
    line-height: 1.6;
    color: var(--text-secondary);
    min-height: 8rem;
    max-height: 14rem;
    overflow-y: auto;
    white-space: pre-wrap;
    word-break: break-all;
  }
  .console-placeholder {
    color: var(--text-muted);
    font-style: italic;
  }
  .console-line {
    color: var(--text-secondary);
  }
</style>
