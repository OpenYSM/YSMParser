/* The frontend of YSMParser.
// Copyright (C) 2026 MiRinChan
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License along
// with this program; if not, see < https://www.gnu.org/licenses/>.
*/

type TauriGlobal = typeof globalThis & {
  __TAURI__?: unknown;
  __TAURI_INTERNALS__?: unknown;
  isTauri?: boolean;
};

const tauriGlobal = globalThis as TauriGlobal;

export const isTauri =
  typeof window !== "undefined" &&
  (tauriGlobal.isTauri === true ||
    "__TAURI__" in tauriGlobal ||
    "__TAURI_INTERNALS__" in tauriGlobal);

let cachedInvoke: typeof import("@tauri-apps/api/core").invoke | null = null;
let cachedDialogOpen: typeof import("@tauri-apps/plugin-dialog").open | null = null;

async function invoke<T>(cmd: string, args?: Record<string, unknown>): Promise<T> {
  if (!cachedInvoke) {
    const mod = await import("@tauri-apps/api/core");
    cachedInvoke = mod.invoke;
  }
  return cachedInvoke(cmd, args) as Promise<T>;
}

async function dialogOpen(): Promise<typeof import("@tauri-apps/plugin-dialog").open> {
  if (!cachedDialogOpen) {
    const mod = await import("@tauri-apps/plugin-dialog");
    cachedDialogOpen = mod.open;
  }
  return cachedDialogOpen;
}

export interface FileStat {
  name: string;
  path: string;
  size: number;
}

export async function runParserNative(
  inputDir: string,
  outputDir: string
): Promise<string> {
  return invoke<string>("run_parser", { inputDir, outputDir });
}

export async function openFolderDialog(): Promise<string | null> {
  return invoke<string | null>("open_folder_dialog");
}

export async function getSavedOutputDir(): Promise<string | null> {
  return invoke<string | null>("get_output_dir");
}

export async function setSavedOutputDir(dir: string): Promise<void> {
  return invoke("set_output_dir", { dir });
}

export async function openPathInFileBrowser(path: string): Promise<void> {
  return invoke("open_in_file_browser", { path });
}

export async function statInputFiles(paths: string[]): Promise<FileStat[]> {
  return invoke<FileStat[]>("stat_input_files", { paths });
}

export async function prepareInputDirFromPaths(paths: string[]): Promise<string> {
  return invoke<string>("prepare_input_dir_from_paths", { paths });
}

export function preloadInputFilesDialog(): void {
  if (isTauri) void dialogOpen();
}

export async function openInputFilesDialog(): Promise<string[]> {
  const open = await dialogOpen();
  const picked = await open({
    multiple: true,
    filters: [{ name: "YSM Files", extensions: ["ysm"] }],
  });
  if (!picked) return [];
  return Array.isArray(picked) ? picked : [picked];
}

export type DragDropPhase = "enter" | "over" | "leave" | "drop";

export async function listenTauriDragDrop(
  onChange: (phase: DragDropPhase, paths: string[]) => void
): Promise<() => void> {
  const { getCurrentWebview } = await import("@tauri-apps/api/webview");
  return getCurrentWebview().onDragDropEvent(({ payload }) => {
    const paths = "paths" in payload ? payload.paths : [];
    onChange(payload.type, paths);
  });
}
