#!/usr/bin/env node
import assert from "node:assert/strict";
import { randomUUID } from "node:crypto";
import { readFile, writeFile } from "node:fs/promises";
import { isAbsolute, join } from "node:path";
import { createInterface } from "node:readline";
import { parseArgs } from "node:util";
import { Client } from "@modelcontextprotocol/client";
import { StdioClientTransport } from "@modelcontextprotocol/client/stdio";
import { verifyLiveSecurity } from "#fixtures/live-security";

const tools = [
  "live.capabilities",
  "live.fx.set_scalar_value",
  "live.inspect",
  "live.receipt",
  "live.stage.set_key_value",
  "live.status",
];
const pending = new Map();
let sequence = 0;
const input = createInterface({ input: process.stdin, crlfDelay: Infinity });
input.on("line", (line) => {
  assert(Buffer.byteLength(line) <= 65536, "Native phase response exceeds 64 KiB");
  const response = JSON.parse(line);
  const request = pending.get(response.id);
  assert(request, "Native fixture returned an unrelated phase response");
  pending.delete(response.id);
  clearTimeout(request.timer);
  request.resolve(response.result);
});

function phase(name) {
  const id = `phase-${++sequence}`;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`Native phase ${name} exceeded 10 seconds`));
    }, 10_000);
    pending.set(id, { resolve, timer });
    process.stdout.write(JSON.stringify({ id, phase: name }) + "\n");
  });
}

function options() {
  const { values } = parseArgs({
    options: {
      executable: { type: "string" },
      adapter: { type: "string" },
      session: { type: "string" },
      evidence: { type: "string" },
    },
    strict: true,
    allowPositionals: false,
  });
  for (const name of ["executable", "adapter", "session", "evidence"]) {
    assert(values[name] && isAbsolute(values[name]), `Fixture ${name} must be absolute`);
  }
  return values;
}

async function connect(context, label) {
  const client = new Client(
    { name: `groundzero-live-${label}`, version: "1.0.0" },
    { versionNegotiation: { mode: { pin: "2026-07-28" } } },
  );
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: [context.adapter, "--executable", context.executable, "--live-session", context.session],
    env: { ...process.env },
    stderr: "inherit",
  });
  const slot = { client, label, closed: false };
  try {
    await client.connect(transport);
    const catalog = await client.listTools();
    assert.equal(catalog.nextCursor, undefined);
    assert.deepEqual(catalog.tools.map((tool) => tool.name).sort(), tools);
    return slot;
  } catch (error) {
    await client.close();
    throw error;
  }
}

async function close(slot, report) {
  if (!slot || slot.closed) return;
  await slot.client.close();
  slot.closed = true;
  report.clients_closed += 1;
}

async function call(slot, name, args, report) {
  const response = await slot.client.callTool({ name, arguments: args });
  assert(response.structuredContent && typeof response.structuredContent === "object");
  assert(
    response.content.every((item) => item.type === "text"),
    "Live tools cannot return images",
  );
  report.calls.push({ client: slot.label, tool: name, args, result: response.structuredContent });
  return response;
}

async function success(slot, name, args, report) {
  const response = await call(slot, name, args, report);
  assert.notEqual(response.isError, true, JSON.stringify(response.structuredContent));
  return response.structuredContent;
}

async function failure(slot, name, args, code, report) {
  const response = await call(slot, name, args, report);
  assert.equal(response.isError, true, "A rejected edit unexpectedly succeeded");
  assert.equal(response.structuredContent.error.code, code);
}

async function inspect(slot, report) {
  const status = await success(slot, "live.status", {}, report);
  assert.equal(typeof status.scene_epoch, "string", JSON.stringify(status));
  const result = await success(slot, "live.inspect", { scene_epoch: status.scene_epoch }, report);
  assert.equal(result.consistency, "qualified_target_dependencies_v1");
  assert.deepEqual(result.unsupported, []);
  return result;
}

function stage(snapshot, kind) {
  const object = snapshot.stage_objects.find((item) => item.kind === kind);
  assert(object, `Missing native ${kind}`);
  const target = snapshot.editable_targets.find(
    (item) => item.object_id === object.object_id && item.channel === "x",
  );
  assert(target, `Missing editable ${kind} x curve`);
  return target;
}

function blur(snapshot, name) {
  const effect = snapshot.fx.find((item) => item.name === name);
  assert(effect, `Missing native ${name}`);
  const target = snapshot.editable_targets.find((item) => item.fx_id === effect.fx_id);
  assert(target, `Missing editable native ${name}`);
  return target;
}

function edit(snapshot, target, value) {
  const request = {
    instance_id: snapshot.instance_id,
    scene_epoch: snapshot.scene_epoch,
    operation_id: randomUUID(),
    expected_fingerprint: target.expected_fingerprint,
    value,
    unit: target.unit,
  };
  if (target.object_id) {
    Object.assign(request, { object_id: target.object_id, channel: target.channel, frame: 10 });
  } else {
    Object.assign(request, {
      fx_id: target.fx_id,
      parameter: target.parameter,
      location: target.location,
    });
    if (target.location === "existing_key") request.frame = 10;
  }
  return request;
}

function changed(result, before, after) {
  assert.equal(result.state, "completed");
  assert.equal(result.changed, true);
  assert.equal(result.undo_entry_added, true);
  assert.equal(result.before_value, before);
  assert.equal(result.after_value, after);
  assert.notEqual(result.before_fingerprint, result.after_fingerprint);
}

function metadataPreserved(before, after, frame, value) {
  const expected = structuredClone(before);
  const key = expected.keys.find((item) => item.frame === frame);
  assert(key, "Expected existing native key is missing");
  key.value = value;
  assert.deepEqual(after, expected, "A scalar edit changed untouched curve metadata");
}

function sameHistory(before, after) {
  assert.equal(after.history_count, before.history_count);
  assert.equal(after.history_index, before.history_index);
  assert.equal(after.dirty, before.dirty);
}

async function stageJourney(first, second, report) {
  const original = await inspect(first, report);
  const peer = await inspect(second, report);
  assert.deepEqual(peer, original, "Two SDK clients did not observe the same handles and snapshot");
  const nativeBefore = (await phase("snapshot")).native;
  const target = stage(original, "camera");
  const request = edit(original, target, 3);
  const applied = await success(first, target.tool, request, report);
  changed(applied, 1, 3);
  const nativeAfter = (await phase("snapshot")).native;
  assert.equal(nativeAfter.history_count, nativeBefore.history_count + 1);
  assert.deepEqual(nativeAfter.camera_x.keys, [
    [0, 0],
    [10, 3],
  ]);
  assert.equal(nativeAfter.camera_x.midpoint, 1.5);
  assert.equal(nativeAfter.dirty, true);
  const observed = await inspect(second, report);
  metadataPreserved(target.curve, stage(observed, "camera").curve, 10, 3);
  await failure(second, target.tool, edit(peer, stage(peer, "camera"), 4), "stale_target", report);
  assert.deepEqual(await success(second, target.tool, request, report), applied);
  await failure(second, target.tool, { ...request, value: 4 }, "operation_conflict", report);
  await redoPreservation(second, report, nativeBefore, nativeAfter);
  await close(first, report);
  assert.deepEqual(await success(second, target.tool, request, report), applied);
  const receipt = await success(
    second,
    "live.receipt",
    { operation_id: request.operation_id },
    report,
  );
  assert.equal(receipt.state, "completed");
  assert.deepEqual(receipt.result, applied);
  assert.equal((await phase("snapshot")).native.history_count, nativeAfter.history_count);
}

async function redoPreservation(client, report, original, applied) {
  const undone = (await phase("human-undo")).native;
  assert.deepEqual(
    undone.camera_x,
    original.camera_x,
    "Native Undo failed to restore the full curve",
  );
  const snapshot = await inspect(client, report);
  const target = stage(snapshot, "camera");
  const noChange = await success(client, target.tool, edit(snapshot, target, 1), report);
  assert.equal(noChange.changed, false);
  assert.equal(noChange.undo_entry_added, false);
  assert.equal(noChange.before_fingerprint, noChange.after_fingerprint);
  await failure(
    client,
    target.tool,
    { ...edit(snapshot, target, 2), unit: "degree" },
    "INVALID_ARGUMENTS",
    report,
  );
  await failure(
    client,
    target.tool,
    { ...edit(snapshot, target, 2), frame: 5 },
    "unsupported_target",
    report,
  );
  const rejected = (await phase("snapshot")).native;
  sameHistory(undone, rejected);
  assert.deepEqual(rejected.camera_x, undone.camera_x);
  const redone = (await phase("human-redo")).native;
  assert.deepEqual(redone.camera_x, applied.camera_x, "Native Redo branch was discarded");
  assert.equal(redone.history_count, applied.history_count);
  assert.equal(redone.history_index, applied.history_index);
}

async function effectJourney(client, report, name, value, nativeField) {
  const snapshot = await inspect(client, report);
  const target = blur(snapshot, name);
  const before = (await phase("snapshot")).native;
  const request = edit(snapshot, target, value);
  const previous = target.location === "default" ? target.curve.default : 6;
  changed(await success(client, target.tool, request, report), previous, value);
  const after = (await phase("snapshot")).native;
  assert.equal(after.history_count, before.history_count + 1);
  const refreshed = blur(await inspect(client, report), name);
  if (target.location === "existing_key") {
    metadataPreserved(target.curve, refreshed.curve, 10, value);
    assert.equal(after[nativeField].midpoint, (2 + value) / 2);
  } else {
    assert.equal(after[nativeField].default, value);
    assert.deepEqual(refreshed.curve, { ...target.curve, default: value });
  }
  const undone = (await phase("human-undo")).native;
  assert.deepEqual(undone[nativeField], before[nativeField]);
  const fresh = await inspect(client, report);
  const wrongLocation = edit(fresh, blur(fresh, name), value);
  if (target.location === "default") {
    wrongLocation.location = "existing_key";
    wrongLocation.frame = 0;
  } else {
    wrongLocation.location = "default";
    delete wrongLocation.frame;
  }
  await failure(client, target.tool, wrongLocation, "unsupported_target", report);
  sameHistory(undone, (await phase("snapshot")).native);
  const redone = (await phase("human-redo")).native;
  assert.deepEqual(redone[nativeField], after[nativeField]);
  assert.equal(redone.history_count, after.history_count);
}

async function staleHumanEdit(client, report) {
  const before = await inspect(client, report);
  const target = stage(before, "pegbar");
  const observed = (await phase("human-stage-edit")).native;
  await failure(client, target.tool, edit(before, target, 5), "stale_target", report);
  const after = (await phase("snapshot")).native;
  assert.deepEqual(after, observed, "Rejected stale edit changed native state or history");
}

async function lockedTarget(client, report, kind) {
  const snapshot = await inspect(client, report);
  const target = stage(snapshot, "camera");
  const before = (await phase("snapshot")).native;
  const locked = (await phase(`${kind}-lock`)).native;
  assert.equal(kind === "camera" ? locked.camera_locked : locked.camera_x.editor_locked, true);
  sameHistory(before, locked);
  await failure(client, target.tool, edit(snapshot, target, 6), "locked", report);
  assert.deepEqual((await phase("snapshot")).native, locked);
  assert.deepEqual((await phase(`${kind}-unlock`)).native, before);
}

async function unsupportedCycle(client, report) {
  const snapshot = await inspect(client, report);
  const target = stage(snapshot, "camera");
  const before = (await phase("snapshot")).native;
  const cycled = (await phase("cycle-enable")).native;
  assert.equal(cycled.camera_x.cycle, true);
  sameHistory(before, cycled);
  const unsupported = await success(
    client,
    "live.inspect",
    { scene_epoch: snapshot.scene_epoch },
    report,
  );
  assert(unsupported.unsupported.length > 0);
  assert.deepEqual(unsupported.editable_targets, []);
  await failure(client, target.tool, edit(snapshot, target, 6), "unsupported_target", report);
  assert.deepEqual((await phase("snapshot")).native, cycled);
  assert.deepEqual((await phase("cycle-disable")).native, before);
}

async function restrictionsPreserveRedo(client, report) {
  const applied = (await phase("snapshot")).native;
  const undone = (await phase("human-undo")).native;
  await lockedTarget(client, report, "camera");
  await lockedTarget(client, report, "curve");
  await unsupportedCycle(client, report);
  assert.deepEqual((await phase("snapshot")).native, undone);
  const redone = (await phase("human-redo")).native;
  assert.deepEqual(redone.keyed_blur, applied.keyed_blur);
  assert.equal(redone.history_count, applied.history_count);
  assert.equal(redone.history_index, applied.history_index);
}

async function dependencyChanges(client, report) {
  let snapshot = await inspect(client, report);
  const camera = stage(snapshot, "camera");
  const previous = (await phase("snapshot")).native;
  const resized = (await phase("camera-size-edit")).native;
  sameHistory(previous, resized);
  assert.equal(resized.camera_width_in, previous.camera_width_in + 0.125);
  await failure(client, camera.tool, edit(snapshot, camera, 4), "stale_target", report);
  snapshot = await inspect(client, report);
  const target = blur(snapshot, "Live default blur");
  const mutated = (await phase("fx-suppressed-edit")).native;
  sameHistory(resized, mutated);
  assert.equal(mutated.blur.default, resized.blur.default + 0.5);
  await failure(client, target.tool, edit(snapshot, target, 5), "stale_target", report);
  assert.deepEqual((await phase("snapshot")).native, mutated);
}

async function excludedArtwork(client, report) {
  const snapshot = await inspect(client, report);
  const before = (await phase("snapshot")).native;
  const changedArtwork = (await phase("excluded-artwork-edit")).native;
  sameHistory(before, changedArtwork);
  assert.notDeepEqual(changedArtwork.art_pixel, before.art_pixel);
  assert.notDeepEqual(changedArtwork.art_palette, before.art_palette);
  assert.deepEqual(changedArtwork.art_pixel, [211, 31, 77, 255]);
  assert.deepEqual(changedArtwork.art_palette, [17, 149, 233, 255]);
  const after = await inspect(client, report);
  assert.deepEqual(
    after.editable_targets,
    snapshot.editable_targets,
    "Excluded artwork content was incorrectly represented as a target dependency",
  );
  assert.equal(after.consistency, "qualified_target_dependencies_v1");
}

async function replacedParameter(client, report) {
  const snapshot = await inspect(client, report);
  const target = blur(snapshot, "Live default blur");
  const before = (await phase("snapshot")).native;
  const replaced = (await phase("fx-parameter-replace")).native;
  assert.deepEqual(replaced, before, "Equal-value parameter replacement changed native values");
  await failure(client, target.tool, edit(snapshot, target, 6), "stale_target", report);
  const fresh = blur(await inspect(client, report), "Live default blur");
  assert.deepEqual(fresh.curve, target.curve);
  assert.notEqual(fresh.expected_fingerprint, target.expected_fingerprint);
  assert.deepEqual((await phase("snapshot")).native, before);
}

async function membershipRetirement(client, report, name, choose) {
  const before = await inspect(client, report);
  const target = choose(before);
  const observed = (await phase(name)).native;
  await failure(client, target.tool, edit(before, target, 7), "target_deleted", report);
  const after = await inspect(client, report);
  const replacement = choose(after);
  assert.notEqual(replacement.object_id ?? replacement.fx_id, target.object_id ?? target.fx_id);
  assert.deepEqual((await phase("snapshot")).native, observed);
}

async function sceneRetirement(client, report) {
  const before = await inspect(client, report);
  const target = stage(before, "camera");
  const absent = (await phase("scene-null")).native;
  assert.equal(absent.scene_present, false);
  await failure(
    client,
    "live.inspect",
    { scene_epoch: before.scene_epoch },
    "scene_changed",
    report,
  );
  await phase("scene-reinstate");
  await failure(client, target.tool, edit(before, target, 9), "scene_changed", report);
  const reinstated = await inspect(client, report);
  assert.notEqual(reinstated.scene_epoch, before.scene_epoch);
  assert.notEqual(stage(reinstated, "camera").object_id, target.object_id);
  await phase("history-reset");
  await failure(
    client,
    "live.inspect",
    { scene_epoch: reinstated.scene_epoch },
    "scene_changed",
    report,
  );
  const reset = await inspect(client, report);
  assert.notEqual(reset.scene_epoch, reinstated.scene_epoch);
  assert.equal((await phase("snapshot")).native.history_count, 0);
}

async function destructiveSceneReplacement(client, report) {
  const before = await inspect(client, report);
  const replaced = (await phase("scene-replace")).native;
  assert.equal(replaced.original_scene_current, false);
  await failure(
    client,
    "live.inspect",
    { scene_epoch: before.scene_epoch },
    "scene_changed",
    report,
  );
  const newScene = await inspect(client, report);
  assert.notEqual(newScene.scene_epoch, before.scene_epoch);
  assert.equal(newScene.editable_targets.length, 0);
  const restored = (await phase("scene-reinstate")).native;
  assert.equal(restored.original_scene_current, true);
  await failure(
    client,
    "live.inspect",
    { scene_epoch: newScene.scene_epoch },
    "scene_changed",
    report,
  );
  const cleared = await inspect(client, report);
  assert.deepEqual(
    cleared.editable_targets,
    [],
    "Native New Scene clears the retired scene before its pointer can be reinstated",
  );
}

async function journey(context, report, slots) {
  assert.equal((await verifyLiveSecurity(context, phase)).status, "passed");
  await connectPair(context, report, slots);
  report.concurrent_clients = 2;
  const [first, second] = slots;
  const capabilities = await success(first, "live.capabilities", {}, report);
  assert.equal(capabilities.mode, "edit");
  assert(capabilities.fingerprint_exclusions.includes("pixels"));
  assert(capabilities.fingerprint_exclusions.includes("palette_colors_and_animation"));
  await stageJourney(first, second, report);
  await effectJourney(second, report, "Live default blur", 4, "blur");
  await effectJourney(second, report, "Live keyed blur", 8, "keyed_blur");
  await restrictionsPreserveRedo(second, report);
  await staleHumanEdit(second, report);
  await dependencyChanges(second, report);
  await replacedParameter(second, report);
  await excludedArtwork(second, report);
  await membershipRetirement(second, report, "peg-reinsert", (snapshot) =>
    stage(snapshot, "pegbar"),
  );
  await membershipRetirement(second, report, "fx-reinsert", (snapshot) =>
    blur(snapshot, "Live default blur"),
  );
  await sceneRetirement(second, report);
  const before = (await phase("snapshot")).native;
  const stopped = (await phase("stop")).native;
  assert.deepEqual(stopped, before, "Stopping the bridge changed native scene state");
  await failure(second, "live.status", {}, "LIVE_UNAVAILABLE", report);
  await close(second, report);
  const stoppedEdit = await stopDuringCommit(context, report, slots);
  await activeShutdown(context, report, slots, stoppedEdit);
}

async function connectPair(context, report, slots) {
  const results = await Promise.allSettled([connect(context, "first"), connect(context, "second")]);
  for (const result of results) {
    if (result.status === "fulfilled") slots.push(result.value);
  }
  report.clients = slots.length;
  for (const result of results) {
    if (result.status === "rejected") throw result.reason;
  }
  report.concurrent_connections_observed = true;
}

async function stopDuringCommit(context, report, slots) {
  await phase("restart");
  const client = await connect(context, "history-stop");
  slots.push(client);
  report.clients = slots.length;
  const snapshot = await inspect(client, report);
  const target = stage(snapshot, "camera");
  const previous = target.curve.keys.find((item) => item.frame === 10).value;
  const value = previous + 2;
  const before = (await phase("arm-stop-on-history")).native;
  const response = await call(client, target.tool, edit(snapshot, target, value), report);
  if (response.isError) {
    assert.equal(response.structuredContent.error.code, "LIVE_UNAVAILABLE");
    assert.equal(response.structuredContent.error.details.outcome_unknown, true);
  } else {
    changed(response.structuredContent, previous, value);
  }
  const observed = await phase("snapshot");
  assert.equal(observed.stop_during_history, true);
  const after = observed.native;
  assert.equal(after.history_count, before.history_count + 1);
  assert.equal(after.history_index, before.history_index + 1);
  assert.deepEqual(after.camera_x.keys, [
    [0, 0],
    [10, value],
  ]);
  assert.equal(after.camera_x.midpoint, value / 2);
  assert.equal(after.dirty, true);
  assert.deepEqual((await phase("human-undo")).native.camera_x, before.camera_x);
  assert.deepEqual((await phase("human-redo")).native.camera_x, after.camera_x);
  assert.equal((await phase("snapshot")).native.history_count, after.history_count);
  await failure(client, "live.status", {}, "LIVE_UNAVAILABLE", report);
  await close(client, report);
  report.stop_during_commit_observed = true;
  return { curve: target.curve, value };
}

async function activeShutdown(context, report, slots, stoppedEdit) {
  await phase("restart");
  const client = await connect(context, "shutdown");
  slots.push(client);
  report.clients = slots.length;
  const snapshot = await inspect(client, report);
  metadataPreserved(stoppedEdit.curve, stage(snapshot, "camera").curve, 10, stoppedEdit.value);
  await destructiveSceneReplacement(client, report);
  const before = (await phase("snapshot")).native;
  assert.deepEqual((await phase("shutdown")).native, before);
  await failure(client, "live.status", {}, "LIVE_UNAVAILABLE", report);
  await assert.rejects(readFile(context.session), { code: "ENOENT" });
  report.active_shutdown_observed = true;
}

async function main() {
  const context = options();
  const report = { status: "failed", clients: 0, clients_closed: 0, calls: [] };
  const slots = [];
  try {
    await journey(context, report, slots);
    report.status = "passed";
  } catch (error) {
    report.error = String(error.stack ?? error);
  } finally {
    const results = await Promise.allSettled(slots.map((slot) => close(slot, report)));
    for (const result of results) {
      if (result.status === "rejected") {
        report.status = "failed";
        report.close_error = String(result.reason);
      }
    }
    await writeFile(join(context.evidence, "sdk.json"), JSON.stringify(report, null, 2) + "\n");
    input.close();
  }
  if (report.status !== "passed") throw new Error(report.error ?? report.close_error);
}

main().catch((error) => {
  process.stderr.write(`live-sdk-fixture: ${error.stack ?? error}\n`);
  process.exitCode = 1;
});
