#!/usr/bin/env node

import { createHash } from "node:crypto";
import { spawn } from "node:child_process";
import {
  access,
  link,
  lstat,
  mkdir,
  mkdtemp,
  readdir,
  readFile,
  rm,
  stat,
  symlink,
} from "node:fs/promises";
import { constants } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { performance } from "node:perf_hooks";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(scriptDir, "../..");
const defaultWasmJs = path.join(
  repoRoot,
  "out/build/wasm-release/YSMParser/YSMParser.js"
);

function parseArgs(argv) {
  const args = {
    mode: "representative",
    representativePerBucket: 8,
    wasmJs: defaultWasmJs,
  };

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    const next = () => {
      if (i + 1 >= argv.length) {
        throw new Error(`Missing value for ${arg}`);
      }
      return argv[++i];
    };

    if (arg === "--mode") args.mode = next();
    else if (arg === "--samples-dir") args.samplesDir = next();
    else if (arg === "--single-file") args.singleFile = next();
    else if (arg === "--wasm-js") args.wasmJs = next();
    else if (arg === "--representative-per-bucket") {
      args.representativePerBucket = Number.parseInt(next(), 10);
    } else if (arg === "--keep-temp") args.keepTemp = true;
    else if (arg === "--help" || arg === "-h") {
      printUsage();
      process.exit(0);
    } else {
      throw new Error(`Unknown argument: ${arg}`);
    }
  }

  if (!["single", "representative", "full"].includes(args.mode)) {
    throw new Error("--mode must be one of: single, representative, full");
  }
  if (args.mode === "single" && !args.singleFile) {
    throw new Error("--single-file is required for --mode single");
  }
  if (args.mode !== "single" && !args.samplesDir) {
    throw new Error("--samples-dir is required for representative/full modes");
  }
  if (!Number.isFinite(args.representativePerBucket) || args.representativePerBucket < 1) {
    throw new Error("--representative-per-bucket must be a positive integer");
  }

  return args;
}

function printUsage() {
  console.error(`Usage:
  node public/scripts/bench-wasm-node.mjs --mode single --single-file /path/model.ysm
  node public/scripts/bench-wasm-node.mjs --mode representative --samples-dir /path/samples
  node public/scripts/bench-wasm-node.mjs --mode full --samples-dir /path/samples

Options:
  --wasm-js PATH                    Defaults to out/build/wasm-release/YSMParser/YSMParser.js
  --representative-per-bucket N     Defaults to 8
  --keep-temp                       Keep temporary input/output directories
`);
}

async function assertReadable(filePath) {
  await access(filePath, constants.R_OK);
}

async function listYsmFiles(samplesDir) {
  const entries = await readdir(samplesDir, { withFileTypes: true });
  const files = [];
  for (const entry of entries) {
    if (!entry.isFile() || !entry.name.toLowerCase().endsWith(".ysm")) continue;
    const fullPath = path.join(samplesDir, entry.name);
    const info = await stat(fullPath);
    files.push({ path: fullPath, size: info.size });
  }
  files.sort((a, b) => a.path.localeCompare(b.path));
  return files;
}

async function hashFile(filePath) {
  const data = await readFile(filePath);
  return createHash("sha256").update(data).digest("hex");
}

async function uniqueByContent(files) {
  const seen = new Set();
  const unique = [];
  for (const file of files) {
    const hash = await hashFile(file.path);
    if (seen.has(hash)) continue;
    seen.add(hash);
    unique.push({ ...file, hash });
  }
  return unique;
}

function bucketName(size) {
  if (size < 100_000) return "lt100k";
  if (size < 1_000_000) return "100k_1m";
  if (size < 5_000_000) return "1m_5m";
  return "ge5m";
}

function pickSpread(files, count) {
  if (files.length <= count) return files;
  if (count === 1) return [files[0]];

  const picked = [];
  const seen = new Set();
  for (let i = 0; i < count; i++) {
    const index = Math.round((i * (files.length - 1)) / (count - 1));
    const file = files[index];
    if (!seen.has(file.path)) {
      seen.add(file.path);
      picked.push(file);
    }
  }
  return picked;
}

async function selectRepresentative(samplesDir, perBucket) {
  const all = await listYsmFiles(samplesDir);
  const unique = await uniqueByContent(all);
  const buckets = new Map();
  for (const file of unique) {
    const name = bucketName(file.size);
    if (!buckets.has(name)) buckets.set(name, []);
    buckets.get(name).push(file);
  }

  const selected = [];
  for (const name of ["lt100k", "100k_1m", "1m_5m", "ge5m"]) {
    const files = buckets.get(name) ?? [];
    files.sort((a, b) => a.size - b.size || a.path.localeCompare(b.path));
    selected.push(...pickSpread(files, perBucket));
  }

  selected.sort((a, b) => a.path.localeCompare(b.path));
  return {
    selected,
    sourceCount: all.length,
    uniqueCount: unique.length,
    bucketCounts: Object.fromEntries(
      [...buckets.entries()].map(([name, files]) => [name, files.length])
    ),
  };
}

async function linkInputFiles(files) {
  const dir = await mkdtemp(path.join(tmpdir(), "ysmparser-wasm-input-"));
  for (let i = 0; i < files.length; i++) {
    const file = files[i];
    const safeName = `${String(i + 1).padStart(4, "0")}-${path.basename(file.path)}`;
    const dest = path.join(dir, safeName);
    try {
      await link(file.path, dest);
    } catch {
      await symlink(file.path, dest);
    }
  }
  return dir;
}

async function countOutputFiles(root) {
  let count = 0;
  let bytes = 0;

  async function walk(dir) {
    const entries = await readdir(dir, { withFileTypes: true });
    for (const entry of entries) {
      const fullPath = path.join(dir, entry.name);
      if (entry.isDirectory()) {
        await walk(fullPath);
      } else if (entry.isFile()) {
        const info = await lstat(fullPath);
        count += 1;
        bytes += info.size;
      }
    }
  }

  await walk(root);
  return { count, bytes };
}

function parseProfileLines(text) {
  const stages = new Map();
  const lines = text.split(/\r?\n/).filter((line) => line.startsWith("YSM_PROFILE\t"));
  for (const line of lines) {
    const fields = new Map();
    for (const part of line.split("\t").slice(1)) {
      const eq = part.indexOf("=");
      if (eq === -1) continue;
      fields.set(part.slice(0, eq), part.slice(eq + 1));
    }
    const stage = fields.get("stage");
    const ms = Number.parseFloat(fields.get("ms") ?? "NaN");
    if (!stage || !Number.isFinite(ms)) continue;
    const current = stages.get(stage) ?? {
      count: 0,
      totalMs: 0,
      minMs: Number.POSITIVE_INFINITY,
      maxMs: 0,
    };
    current.count += 1;
    current.totalMs += ms;
    current.minMs = Math.min(current.minMs, ms);
    current.maxMs = Math.max(current.maxMs, ms);
    stages.set(stage, current);
  }

  return Object.fromEntries(
    [...stages.entries()].map(([stage, data]) => [
      stage,
      {
        count: data.count,
        totalMs: Number(data.totalMs.toFixed(3)),
        avgMs: Number((data.totalMs / data.count).toFixed(3)),
        minMs: Number(data.minMs.toFixed(3)),
        maxMs: Number(data.maxMs.toFixed(3)),
      },
    ])
  );
}

async function runParser({ wasmJs, inputDir, outputDir }) {
  await mkdir(outputDir, { recursive: true });
  const args = [wasmJs, "-i", inputDir, "-o", outputDir, "--profile"];
  const start = performance.now();
  const child = spawn(process.execPath, args, {
    cwd: repoRoot,
    stdio: ["ignore", "pipe", "pipe"],
  });

  let stdout = "";
  let stderr = "";
  child.stdout.setEncoding("utf8");
  child.stderr.setEncoding("utf8");
  child.stdout.on("data", (chunk) => {
    stdout += chunk;
  });
  child.stderr.on("data", (chunk) => {
    stderr += chunk;
  });

  const status = await new Promise((resolve, reject) => {
    child.on("error", reject);
    child.on("close", (code, signal) => resolve({ code, signal }));
  });
  const wallMs = performance.now() - start;
  const output = await countOutputFiles(outputDir);

  return {
    command: [process.execPath, ...args],
    status,
    wallMs,
    stdoutBytes: Buffer.byteLength(stdout),
    stderrBytes: Buffer.byteLength(stderr),
    profile: parseProfileLines(`${stdout}\n${stderr}`),
    output,
    tail: `${stdout}\n${stderr}`.trim().split(/\r?\n/).slice(-20),
  };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  await assertReadable(args.wasmJs);

  let inputDir = null;
  let outputDir = null;
  let tempInputDir = null;
  let metadata = {};
  let files = [];

  try {
    if (args.mode === "single") {
      const info = await stat(args.singleFile);
      files = [{ path: args.singleFile, size: info.size }];
      tempInputDir = await linkInputFiles(files);
      inputDir = tempInputDir;
    } else if (args.mode === "representative") {
      metadata = await selectRepresentative(args.samplesDir, args.representativePerBucket);
      files = metadata.selected;
      tempInputDir = await linkInputFiles(files);
      inputDir = tempInputDir;
    } else {
      files = await listYsmFiles(args.samplesDir);
      inputDir = args.samplesDir;
      metadata = { sourceCount: files.length };
    }

    outputDir = await mkdtemp(path.join(tmpdir(), "ysmparser-wasm-output-"));
    const run = await runParser({ wasmJs: args.wasmJs, inputDir, outputDir });
    const result = {
      mode: args.mode,
      wasmJs: args.wasmJs,
      inputDir,
      outputDir,
      fileCount: files.length,
      inputBytes: files.reduce((sum, file) => sum + file.size, 0),
      ...metadata,
      run,
    };
    console.log(JSON.stringify(result, null, 2));

    if (run.status.code !== 0) {
      process.exitCode = run.status.code ?? 1;
    }
  } finally {
    if (!args.keepTemp) {
      if (tempInputDir) await rm(tempInputDir, { recursive: true, force: true });
      if (outputDir) await rm(outputDir, { recursive: true, force: true });
    }
  }
}

main().catch((error) => {
  console.error(error instanceof Error ? error.message : String(error));
  process.exit(1);
});
