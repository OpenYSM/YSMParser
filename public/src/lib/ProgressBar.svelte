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
  interface Props {
    percent: number;
    label?: string;
  }
  let { percent, label }: Props = $props();
  let clamped = $derived(Math.max(0, Math.min(100, percent)));
</script>

<div class="progress-wrap">
  <div
    class="progress-track"
    role="progressbar"
    aria-valuenow={Math.round(clamped)}
    aria-valuemin="0"
    aria-valuemax="100"
    aria-label={label ?? "Progress"}
  >
    <div class="progress-fill" style="width: {clamped}%"></div>
  </div>
  {#if label}
    <span class="progress-label">{label}</span>
  {/if}
</div>

<style>
  .progress-wrap {
    display: flex;
    align-items: center;
    gap: 0.75rem;
  }
  .progress-track {
    flex: 1;
    height: 6px;
    background: var(--surface-2);
    border-radius: 999px;
    overflow: hidden;
  }
  .progress-fill {
    height: 100%;
    background: var(--accent);
    border-radius: 999px;
    transition: width 0.25s ease;
  }
  .progress-label {
    font-size: 0.75rem;
    font-family: var(--font-mono);
    color: var(--text-muted);
    min-width: 6rem;
    text-align: right;
  }
</style>
