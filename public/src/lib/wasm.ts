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

declare global {
  interface Window {
    YSMParserModule?: (opts: unknown) => Promise<WasmModule>;
    Module?: (opts: unknown) => Promise<WasmModule>;
  }
}

export interface WasmModule {
  FS: {
    mkdir(path: string): void;
    readdir(path: string): string[];
    readFile(path: string): Uint8Array;
    writeFile(path: string, data: Uint8Array): void;
    unlink(path: string): void;
    rmdir(path: string): void;
    stat(path: string): { mode: number };
    isDir(mode: number): boolean;
  };
  callMain(args: string[]): number;
}

export interface OutputFile {
  path: string;
  data: Uint8Array;
}

export interface RunWasmOptions {
  maxBatchBytes?: number;
  onOutputFile?: (file: OutputFile) => void;
  onProgress: (pct: number, label: string) => void;
}

export interface RunWasmResult {
  outputCount: number;
  outputBytes: number;
  batchCount: number;
  timings: RunWasmTimings;
}

export interface RunWasmBatchTiming {
  batch: number;
  fileCount: number;
  inputBytes: number;
  inputWriteMs: number;
  parserMs: number;
  outputCollectMs: number;
  cleanupMs: number;
  totalMs: number;
}

export interface RunWasmTimings {
  inputWriteMs: number;
  parserMs: number;
  outputCollectMs: number;
  cleanupMs: number;
  totalMs: number;
  batches: RunWasmBatchTiming[];
}

const DEFAULT_MAX_BATCH_BYTES = 96 * 1024 * 1024;
let runtimeScriptPromise: Promise<void> | null = null;

function wasmFactory(): ((opts: unknown) => Promise<WasmModule>) | undefined {
  const factory =
    window.YSMParserModule ??
    window.Module ??
    // @ts-ignore
    globalThis.YSMParserModule ??
    // @ts-ignore
    globalThis.Module;
  return typeof factory === "function" ? factory : undefined;
}

export async function initWasm(
  onLog: (text: string) => void
): Promise<WasmModule> {
  await loadWasmRuntimeScript();
  const factory = wasmFactory();

  if (!factory) {
    const msg = await diagnoseFactoryError();
    throw new Error(msg);
  }

  return factory({
    noInitialRun: true,
    print: (text: string) => onLog(text),
    printErr: (text: string) => onLog(text),
    locateFile: (path: string) => `./${path}`,
  });
}

function loadWasmRuntimeScript(): Promise<void> {
  if (wasmFactory()) return Promise.resolve();
  if (runtimeScriptPromise) return runtimeScriptPromise;

  runtimeScriptPromise = new Promise((resolve, reject) => {
    const existing = document.querySelector<HTMLScriptElement>(
      'script[data-ysmparser-runtime="true"]'
    );
    if (existing) {
      existing.addEventListener("load", () => resolve(), { once: true });
      existing.addEventListener("error", () => reject(new Error("Failed to load YSMParser.js")), {
        once: true,
      });
      return;
    }

    const script = document.createElement("script");
    script.src = "./YSMParser.js";
    script.async = true;
    script.dataset.ysmparserRuntime = "true";
    script.onload = () => resolve();
    script.onerror = () => reject(new Error("Failed to load YSMParser.js"));
    document.head.appendChild(script);
  });

  return runtimeScriptPromise;
}

async function diagnoseFactoryError(): Promise<string> {
  try {
    const response = await fetch("./YSMParser.js", { cache: "no-store" });
    if (response.ok) {
      const source = await response.text();
      if (
        source.includes('require("node:fs")') ||
        source.includes("require('node:fs')") ||
        source.includes("NODERAWFS") ||
        source.includes("ENVIRONMENT_IS_NODE=true")
      ) {
        return "WASM file mismatch — place the web build of YSMParser.js alongside this page.";
      }
    }
  } catch {
    // ignore
  }
  return "WASM runtime not available — make sure YSMParser.js and YSMParser.wasm are in the same directory.";
}

function wipeDir(FS: WasmModule["FS"], dir: string): void {
  try {
    const entries = FS.readdir(dir).filter((n) => n !== "." && n !== "..");
    for (const entry of entries) {
      const full = `${dir}/${entry}`;
      const stat = FS.stat(full);
      if (FS.isDir(stat.mode)) {
        wipeDir(FS, full);
        FS.rmdir(full);
      } else {
        FS.unlink(full);
      }
    }
  } catch {
    // ignore
  }
}

function ensureDir(FS: WasmModule["FS"], dir: string): void {
  const parts = dir.split("/").filter(Boolean);
  let current = "";
  for (const part of parts) {
    current += `/${part}`;
    try {
      FS.mkdir(current);
    } catch {
      // already exists
    }
  }
}

export async function runWasm(
  mod: WasmModule,
  files: File[],
  options: RunWasmOptions
): Promise<RunWasmResult> {
  const { FS } = mod;
  const batches = makeFileBatches(
    files,
    options.maxBatchBytes ?? DEFAULT_MAX_BATCH_BYTES
  );
  let outputCount = 0;
  let outputBytes = 0;
  const timings: RunWasmTimings = {
    inputWriteMs: 0,
    parserMs: 0,
    outputCollectMs: 0,
    cleanupMs: 0,
    totalMs: 0,
    batches: [],
  };

  if (batches.length === 0) {
    return { outputCount: 0, outputBytes: 0, batchCount: 0, timings };
  }

  const totalStart = performance.now();
  for (let batchIndex = 0; batchIndex < batches.length; batchIndex++) {
    const batch = batches[batchIndex];
    const batchNumber = batchIndex + 1;
    const batchBase = (batchIndex / batches.length) * 75;
    const batchSpan = 75 / batches.length;
    const batchInputBytes = batch.reduce((sum, file) => sum + file.size, 0);
    const batchStart = performance.now();

    wipeDir(FS, "/input");
    wipeDir(FS, "/output");
    ensureDir(FS, "/input");
    ensureDir(FS, "/output");

    const inputStart = performance.now();
    for (let i = 0; i < batch.length; i++) {
      const file = batch[i];
      const bytes = new Uint8Array(await file.arrayBuffer());
      FS.writeFile(`/input/${file.name}`, bytes);
      options.onProgress(
        batchBase + (i / Math.max(1, batch.length)) * batchSpan * 0.35,
        `Loading batch ${batchNumber} / ${batches.length}`
      );
    }
    const inputWriteMs = performance.now() - inputStart;
    timings.inputWriteMs += inputWriteMs;

    options.onProgress(
      batchBase + batchSpan * 0.35,
      `Parsing batch ${batchNumber} / ${batches.length}`
    );
    const parserStart = performance.now();
    const exitCode = mod.callMain(["-i", "/input", "-o", "/output", "--profile"]);
    const parserMs = performance.now() - parserStart;
    timings.parserMs += parserMs;
    if (typeof exitCode === "number" && exitCode !== 0) {
      throw new Error(`Parser exited with code ${exitCode}`);
    }

    options.onProgress(
      batchBase + batchSpan * 0.9,
      `Collecting batch ${batchNumber} / ${batches.length}`
    );
    const outputStart = performance.now();
    const batchOutput = collectOutputFiles(FS, "/output", options.onOutputFile);
    const outputCollectMs = performance.now() - outputStart;
    timings.outputCollectMs += outputCollectMs;
    outputCount += batchOutput.count;
    outputBytes += batchOutput.bytes;

    const cleanupStart = performance.now();
    wipeDir(FS, "/input");
    wipeDir(FS, "/output");
    const cleanupMs = performance.now() - cleanupStart;
    timings.cleanupMs += cleanupMs;
    timings.batches.push({
      batch: batchNumber,
      fileCount: batch.length,
      inputBytes: batchInputBytes,
      inputWriteMs,
      parserMs,
      outputCollectMs,
      cleanupMs,
      totalMs: performance.now() - batchStart,
    });
    options.onProgress(
      batchBase + batchSpan,
      `Finished batch ${batchNumber} / ${batches.length}`
    );
  }

  timings.totalMs = performance.now() - totalStart;
  return { outputCount, outputBytes, batchCount: batches.length, timings };
}

function makeFileBatches(files: File[], maxBatchBytes: number): File[][] {
  const safeMax = Math.max(1, maxBatchBytes);
  const batches: File[][] = [];
  let batch: File[] = [];
  let batchBytes = 0;

  for (const file of files) {
    if (batch.length > 0 && batchBytes + file.size > safeMax) {
      batches.push(batch);
      batch = [];
      batchBytes = 0;
    }

    batch.push(file);
    batchBytes += file.size;
  }

  if (batch.length > 0) {
    batches.push(batch);
  }

  return batches;
}

function collectOutputFiles(
  FS: WasmModule["FS"],
  root: string,
  onOutputFile?: (file: OutputFile) => void
): { count: number; bytes: number } {
  let count = 0;
  let bytes = 0;
  const walk = (dir: string, relativeBase: string) => {
    const entries = FS.readdir(dir).filter((n) => n !== "." && n !== "..");
    for (const entry of entries) {
      const fullPath = `${dir}/${entry}`;
      const relPath = relativeBase ? `${relativeBase}/${entry}` : entry;
      const stat = FS.stat(fullPath);
      if (FS.isDir(stat.mode)) {
        walk(fullPath, relPath);
      } else {
        const data = FS.readFile(fullPath);
        count += 1;
        bytes += data.byteLength;
        onOutputFile?.({ path: relPath, data });
      }
    }
  };
  walk(root, "");
  return { count, bytes };
}
