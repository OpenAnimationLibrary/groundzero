import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { once } from "node:events";
import {
  chmod,
  copyFile,
  link,
  mkdtemp,
  readFile,
  rename,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import { createConnection } from "node:net";
import { tmpdir } from "node:os";
import { basename, dirname, isAbsolute, join } from "node:path";
import { setTimeout as delay } from "node:timers/promises";

const protocol = "groundzero-live-v2";
const catalog = [
  "live.capabilities",
  "live.fx.set_scalar_value",
  "live.inspect",
  "live.receipt",
  "live.stage.set_key_value",
  "live.status",
];

async function bounded(promise, label, milliseconds = 5000) {
  let timer;
  try {
    return await Promise.race([
      promise,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error(`${label} exceeded its deadline`)), milliseconds);
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

function frame(value) {
  const payload = Buffer.isBuffer(value) ? value : Buffer.from(JSON.stringify(value));
  const header = Buffer.alloc(4);
  header.writeUInt32BE(payload.length);
  return Buffer.concat([header, payload]);
}

class Peer {
  constructor(endpoint) {
    this.socket = createConnection(endpoint);
    this.buffer = Buffer.alloc(0);
    this.frames = [];
    this.waiters = [];
    this.closed = false;
    this.failure = undefined;
    this.connected = once(this.socket, "connect");
    this.finished = new Promise((resolve) => this.socket.once("close", resolve));
    this.socket.on("error", (error) => this.fail(error));
    this.socket.on("close", () => {
      this.closed = true;
      this.fail(new Error("Native live peer closed the connection"));
    });
    this.socket.on("data", (chunk) => {
      try {
        this.receive(chunk);
      } catch (error) {
        this.fail(error);
        this.socket.destroy();
      }
    });
  }

  fail(error) {
    this.failure ??= error;
    for (const waiter of this.waiters.splice(0)) waiter.reject(error);
  }

  receive(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (this.buffer.length >= 4) {
      const length = this.buffer.readUInt32BE(0);
      assert(length > 0 && length <= 1024 * 1024, "Native response violates its frame bound");
      if (this.buffer.length < length + 4) return;
      const bytes = this.buffer.subarray(4, length + 4);
      this.buffer = this.buffer.subarray(length + 4);
      const value = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
      const waiter = this.waiters.shift();
      if (waiter) waiter.resolve(value);
      else this.frames.push(value);
    }
  }

  async open() {
    try {
      await bounded(this.connected, "Native local connection");
      return this;
    } catch (error) {
      await this.close();
      throw error;
    }
  }

  read() {
    if (this.frames.length > 0) return Promise.resolve(this.frames.shift());
    if (this.failure) return Promise.reject(this.failure);
    return bounded(
      new Promise((resolve, reject) => this.waiters.push({ resolve, reject })),
      "Native framed response",
    );
  }

  async close() {
    this.socket.destroy();
    await bounded(this.finished, "Native peer cleanup");
  }
}

async function descriptor(path) {
  const bytes = await readFile(path);
  assert(bytes.length <= 4096, "Published session descriptor exceeds 4096 bytes");
  const value = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
  assert.deepEqual(Object.keys(value).sort(), [
    "build",
    "endpoint",
    "instance_id",
    "mode",
    "pid",
    "protocol",
    "token",
  ]);
  assert.equal(value.protocol, protocol);
  assert.deepEqual(Object.keys(value.build).sort(), ["application", "build_digest", "version"]);
  assert.match(value.build.build_digest, /^[0-9a-f]{64}$/);
  assert.match(value.token, /^[0-9a-f]{64}$/);
  return value;
}

function authentication(session) {
  return {
    protocol,
    build: session.build,
    instance_id: session.instance_id,
    client_id: randomUUID(),
    token: session.token,
  };
}

function checkHello(hello, session) {
  assert.equal(hello.protocol, protocol);
  assert.deepEqual(hello.build, session.build);
  assert.equal(hello.instance_id, session.instance_id);
  assert.equal(hello.mode, session.mode);
  assert.equal(typeof hello.client_id, "string");
  assert.deepEqual(hello.tools.map((tool) => tool.name).sort(), catalog);
  assert.equal(typeof hello.limits, "object");
  assert(!JSON.stringify(hello).includes(session.token), "Authentication leaked its token");
}

async function authenticated(session) {
  const peer = await new Peer(session.endpoint).open();
  try {
    const request = authentication(session);
    peer.socket.write(frame(request));
    const hello = await peer.read();
    checkHello(hello, session);
    assert.equal(hello.client_id, request.client_id);
    return peer;
  } catch (error) {
    await peer.close();
    throw error;
  }
}

async function request(peer, method, params = {}) {
  const id = randomUUID();
  peer.socket.write(frame({ id, method, params }));
  const response = await peer.read();
  assert.equal(response.id, id);
  assert.notEqual("result" in response, "error" in response);
  return response;
}

async function rejectedAuthentication(session, payload, label) {
  const peer = await new Peer(session.endpoint).open();
  try {
    peer.socket.write(payload);
    await bounded(peer.finished, label, 4500);
    assert(!JSON.stringify(peer.frames).includes(session.token), `${label} leaked a token`);
    for (const response of peer.frames) {
      assert(!("result" in response), `${label} exposed a result before authentication`);
      assert(
        ["authentication_failed", "invalid_params", ...Object.values(mismatches)].includes(
          response.error?.code,
        ),
        `${label} exposed unexpected preauthentication output`,
      );
    }
    if (label in mismatches) {
      assert.equal(peer.frames.length, 1, `${label} was not answered with its refusal`);
      assert.equal(peer.frames[0].error.code, mismatches[label]);
      assert.deepEqual(Object.keys(peer.frames[0].error.details.editor).sort(), [
        "build",
        "protocol",
      ]);
    }
  } finally {
    await peer.close();
  }
}

const mismatches = {
  "wrong protocol": "protocol_mismatch",
  "foreign build and wrong token": "build_mismatch",
};

async function authenticationFailures(session) {
  const valid = authentication(session);
  const badToken = (session.token[0] === "0" ? "1" : "0") + session.token.slice(1);
  const cases = [
    ["wrong token", frame({ ...valid, token: badToken })],
    ["wrong instance", frame({ ...valid, instance_id: randomUUID() })],
    ["wrong protocol", frame({ ...valid, protocol: "unsupported-live-version" })],
    [
      "foreign build and wrong token",
      frame({
        ...valid,
        token: badToken,
        build: { ...valid.build, build_digest: "f".repeat(64) },
      }),
    ],
    ["unknown authentication field", frame({ ...valid, unexpected: true })],
    ["unauthenticated tool", frame({ id: "before-auth", method: "live.status", params: {} })],
    ["duplicate authentication key", frame(Buffer.from('{"protocol":"a","protocol":"b"}'))],
    ["invalid UTF-8", frame(Buffer.from([0xc3, 0x28]))],
    ["empty frame", Buffer.alloc(4)],
    ["oversized frame", Buffer.from([0, 1, 0, 1])],
  ];
  for (const [label, payload] of cases) await rejectedAuthentication(session, payload, label);
  await rejectedAuthentication(session, Buffer.alloc(0), "Authentication deadline");
  return cases.length + 1;
}

function processOutcome(child) {
  return new Promise((resolve, reject) => {
    child.once("error", reject);
    child.once("close", (code, signal) => resolve({ code, signal }));
  });
}

function processHello(stream) {
  return new Promise((resolve, reject) => {
    let bytes = Buffer.alloc(0);
    const receive = (chunk) => {
      try {
        bytes = Buffer.concat([bytes, chunk]);
        if (bytes.length < 4) return;
        const length = bytes.readUInt32BE(0);
        assert(length > 0 && length <= 1024 * 1024, "Connector hello exceeds frame bounds");
        if (bytes.length < length + 4) return;
        stream.off("data", receive);
        resolve(JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes.subarray(4))));
      } catch (error) {
        stream.off("data", receive);
        reject(error);
      }
    };
    stream.on("data", receive);
    stream.once("error", reject);
  });
}

async function connectorBaseline(context, session, report) {
  const child = spawn(context.executable, ["--automation-live-connect", context.session], {
    stdio: ["pipe", "pipe", "pipe"],
  });
  const outcome = processOutcome(child);
  child.stderr.resume();
  try {
    const hello = await bounded(
      Promise.race([
        processHello(child.stdout),
        outcome.then(() => {
          throw new Error("Native connector exited before its authenticated hello");
        }),
      ]),
      "Native connector authenticated hello",
    );
    checkHello(hello, session);
    if (report) report.hello_build = hello.build;
    child.stdin.end();
    const result = await bounded(outcome, "Native connector EOF");
    assert.deepEqual(result, { code: 0, signal: null });
  } finally {
    if (child.exitCode === null && child.signalCode === null) child.kill("SIGKILL");
    await bounded(outcome, "Native connector cleanup");
  }
}

function refusalFrame(stdout, code, label) {
  assert(stdout.length > 4, `${label} sent no refusal frame`);
  assert.equal(stdout.readUInt32BE(0), stdout.length - 4, `${label} sent more than one frame`);
  const value = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(stdout.subarray(4)));
  assert.deepEqual(Object.keys(value), ["refusal"]);
  assert.deepEqual(Object.keys(value.refusal).sort(), ["code", "details", "message"]);
  if (code) assert.equal(value.refusal.code, code, `${label} refused with another code`);
  return value.refusal;
}

async function rejectedFile(executable, path, token, label, code = undefined) {
  const child = spawn(executable, ["--automation-live-connect", path], {
    stdio: ["pipe", "pipe", "pipe"],
  });
  const outcome = processOutcome(child);
  const output = [];
  const errors = [];
  child.stdout.on("data", (chunk) => output.push(chunk));
  child.stderr.on("data", (chunk) => errors.push(chunk));
  child.stdin.end();
  try {
    const result = await bounded(outcome, label);
    const stdout = Buffer.concat(output);
    const stderr = Buffer.concat(errors).toString("utf8");
    assert.equal(result.code, 2, `${label} was not refused by native session admission`);
    assert.equal(result.signal, null, `${label} crashed the native connector`);
    assert.equal(stderr, "", `${label} wrote diagnostics instead of a refusal frame`);
    assert(!stdout.includes(token), `${label} leaked a token in its refusal`);
    return refusalFrame(stdout, code, label);
  } finally {
    if (child.exitCode === null && child.signalCode === null) child.kill("SIGKILL");
    await bounded(outcome, `${label} process cleanup`);
  }
}

const windowsProtocol = "groundzero-windows-live-ci-v1";

async function controlObject(path) {
  const bytes = await readFile(path);
  assert(bytes.length <= 16384, "Windows fixture control exceeds 16 KiB");
  const value = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
  assert(value && typeof value === "object" && !Array.isArray(value));
  return value;
}

async function windowsOwner(control) {
  const configuration = await controlObject(join(control, "context.json"));
  assert.deepEqual(Object.keys(configuration).sort(), [
    "launcher_sid",
    "owner_logon_sha256",
    "owner_user_sha256",
    "protocol",
  ]);
  assert.equal(configuration.protocol, windowsProtocol);
  const report = await controlObject(join(control, "owner.json"));
  assert.equal(report.status, "passed");
  const owner = report.identity;
  assert.equal(owner.elevated, false);
  for (const field of ["user_sha256", "logon_sha256"]) {
    assert.match(owner[field], /^[0-9a-f]{64}$/);
    assert.equal(configuration[`owner_${field}`], owner[field]);
  }
  assert(Number.isInteger(owner.pid) && owner.pid > 0);
  return owner;
}

async function peerResponse(path) {
  const deadline = performance.now() + 25000;
  while (performance.now() < deadline) {
    try {
      return await controlObject(path);
    } catch (error) {
      if (error.code !== "ENOENT") throw error;
    }
    await delay(20);
  }
  throw new Error("Windows native peer probes exceeded 25 seconds");
}

function verifyIdentityDenials(response, owner) {
  assert.deepEqual(Object.keys(response).sort(), ["id", "status", "wrong_logon", "wrong_user"]);
  assert.equal(response.status, "passed");
  for (const name of ["wrong_user", "wrong_logon"]) {
    const report = response[name];
    assert.equal(report.status, "passed");
    assert.equal(report.identity.elevated, false);
    assert.notEqual(report.identity.logon_sha256, owner.logon_sha256);
    assert(Number.isInteger(report.identity.pid) && report.identity.pid > 0);
  }
  assert.notEqual(response.wrong_user.identity.user_sha256, owner.user_sha256);
  assert.equal(response.wrong_logon.identity.user_sha256, owner.user_sha256);
  assert.deepEqual(response.wrong_user.probe, { denial: "ERROR_ACCESS_DENIED", win32_error: 5 });
  assert.deepEqual(response.wrong_logon.probe, {
    denial: "live_security",
    connector_exit_code: 1,
    connector_stdout_bytes: 0,
  });
}

async function windowsIdentities(context, session) {
  assert.equal(process.env.GITHUB_ACTIONS, "true");
  assert.equal(process.env.RUNNER_ENVIRONMENT, "github-hosted");
  assert.equal(process.env.RUNNER_OS, "Windows");
  const control = process.env.GROUNDZERO_WINDOWS_LIVE_CONTROL;
  assert(
    control && isAbsolute(control),
    "Hosted Windows qualification requires its fixed launcher",
  );
  const owner = await windowsOwner(control);
  await connectorBaseline(context, session);
  const id = randomUUID();
  const request = {
    protocol: windowsProtocol,
    kind: "identity",
    id,
    session_path: context.session,
    endpoint: session.endpoint,
  };
  await assert.rejects(readFile(join(control, "peer-request.json")), { code: "ENOENT" });
  await assert.rejects(readFile(join(control, "peer-response.json")), { code: "ENOENT" });
  const temporary = join(control, `peer-request-${id}.tmp`);
  await writeFile(temporary, JSON.stringify(request), { flag: "wx" });
  await rename(temporary, join(control, "peer-request.json"));
  const response = await bounded(
    peerResponse(join(control, "peer-response.json")),
    "Windows native identity probes",
    26000,
  );
  assert.equal(response.id, id);
  verifyIdentityDenials(response, owner);
  assert.deepEqual(await descriptor(context.session), session);
  await connectorBaseline(context, session);
  return {
    owner,
    wrong_user: response.wrong_user,
    wrong_logon: response.wrong_logon,
    positive_connectors: 2,
  };
}

async function posixFiles(context, session) {
  const root = await mkdtemp(join(tmpdir(), "gz-live-security-"));
  await chmod(root, 0o700);
  const directory = dirname(context.session);
  const prefix = `fixture-${randomUUID()}`;
  const paths = ["group", "linked", "hardlink", "symbolic", "alias", "foreign"].map((name) =>
    join(directory, `${prefix}-${name}.json`),
  );
  const [group, linked, hardlink, symbolic, aliased, foreign] = paths;
  try {
    for (const path of [group, linked, aliased]) {
      await copyFile(context.session, path);
      await chmod(path, 0o600);
      await connectorBaseline({ ...context, session: path }, session);
    }
    await chmod(group, 0o640);
    await rejectedFile(context.executable, group, session.token, "group-readable session file");
    await link(linked, hardlink);
    await rejectedFile(context.executable, hardlink, session.token, "multiply linked session file");
    await symlink(context.session, symbolic);
    await rejectedFile(context.executable, symbolic, session.token, "symbolic session file");
    const alias = join(root, "directory-alias");
    await symlink(directory, alias);
    await writeFile(
      aliased,
      JSON.stringify({ ...session, endpoint: join(alias, basename(session.endpoint)) }),
    );
    await rejectedFile(
      context.executable,
      join(alias, basename(aliased)),
      session.token,
      "symbolic directory ancestor",
    );
    await chmod(directory, 0o755);
    try {
      await rejectedFile(
        context.executable,
        context.session,
        session.token,
        "public session directory",
      );
    } finally {
      await chmod(directory, 0o700);
    }
    await rejectedFile(
      context.executable,
      root,
      session.token,
      "directory in place of session file",
    );
    await foreignBuild(context, session, foreign);
    return 7;
  } finally {
    await chmod(directory, 0o700);
    for (const path of paths) await rm(path, { force: true });
    await rm(root, { recursive: true });
  }
}

async function foreignBuild(context, session, path) {
  const build = { ...session.build, build_digest: "e".repeat(64) };
  await writeFile(path, JSON.stringify({ ...session, build }), { mode: 0o600 });
  const refusal = await rejectedFile(
    context.executable,
    path,
    session.token,
    "session file of another build",
    "build_mismatch",
  );
  assert.deepEqual(refusal.details.editor, { protocol, build });
  assert.deepEqual(refusal.details.connector, { protocol, build: session.build });
  assert(!JSON.stringify(refusal).includes(session.endpoint), "A refusal named the endpoint");
}

async function rejectedFrame(session, payload, label) {
  const peer = await authenticated(session);
  try {
    peer.socket.write(payload);
    await bounded(peer.finished, label);
    assert.equal(peer.frames.length, 0, `${label} published a native result`);
  } finally {
    await peer.close();
  }
}

async function framingCases(session) {
  const deep = `${'{"child":'.repeat(17)}0${"}".repeat(17)}`;
  const cases = [
    [
      "duplicate request key",
      frame(Buffer.from('{"id":"x","method":"live.status","params":{},"params":{}}')),
    ],
    ["batch request", frame([])],
    ["deep request", frame(Buffer.from(deep))],
    ["invalid request UTF-8", frame(Buffer.from([0xc3, 0x28]))],
    ["oversized authenticated frame", Buffer.from([0, 1, 0, 1])],
    ["empty authenticated frame", Buffer.alloc(4)],
  ];
  for (const [label, payload] of cases) await rejectedFrame(session, payload, label);
  const peer = await authenticated(session);
  try {
    assert.equal(
      (await request(peer, "live.status", { unknown: true })).error.code,
      "invalid_params",
    );
    assert((await request(peer, "live.capabilities")).result);
  } finally {
    await peer.close();
  }
  return cases.length + 1;
}

async function connectionLimits(session) {
  const peers = [];
  try {
    peers.push(await authenticated(session));
    peers.push(await authenticated(session));
    await rejectedAuthentication(
      session,
      frame(authentication(session)),
      "Third authenticated client",
    );
    assert((await request(peers[0], "live.status")).result);
    assert((await request(peers[1], "live.status")).result);
  } finally {
    await Promise.all(peers.map((peer) => peer.close()));
  }
  const waiting = [];
  try {
    waiting.push(await new Peer(session.endpoint).open());
    waiting.push(await new Peer(session.endpoint).open());
    await rejectedAuthentication(
      session,
      frame(authentication(session)),
      "Third unauthenticated client",
    );
  } finally {
    await Promise.all(waiting.map((peer) => peer.close()));
  }
}

async function inspectPeer(peer) {
  const status = (await request(peer, "live.status")).result;
  assert.equal(typeof status.scene_epoch, "string");
  const response = await request(peer, "live.inspect", { scene_epoch: status.scene_epoch });
  assert(response.result, "Native inspection failed before the boundary case");
  return response.result;
}

function cameraEdit(snapshot) {
  const camera = snapshot.stage_objects.find((item) => item.kind === "camera");
  const target = snapshot.editable_targets.find(
    (item) => item.object_id === camera.object_id && item.channel === "x",
  );
  assert(target, "Fixed security fixture camera key is missing");
  const key = target.curve.keys.find((item) => item.frame === 10);
  return {
    instance_id: snapshot.instance_id,
    scene_epoch: snapshot.scene_epoch,
    operation_id: randomUUID(),
    expected_fingerprint: target.expected_fingerprint,
    object_id: target.object_id,
    channel: "x",
    unit: "inch",
    frame: 10,
    value: key.value + 1,
  };
}

async function inspectMode(context, phase) {
  const before = (await phase("inspect-mode")).native;
  const session = await descriptor(context.session);
  assert.equal(session.mode, "inspect");
  const peer = await authenticated(session);
  try {
    const snapshot = await inspectPeer(peer);
    const operation = cameraEdit(snapshot);
    const rejected = await request(peer, "live.stage.set_key_value", operation);
    assert.equal(rejected.error.code, "permission_denied");
    const receipt = (await request(peer, "live.receipt", { operation_id: operation.operation_id }))
      .result;
    assert.equal(receipt.state, "rejected");
    assert.equal(receipt.error.code, "permission_denied");
    assert.deepEqual((await phase("snapshot")).native, before);
  } finally {
    await peer.close();
    await phase("edit-mode");
  }
}

async function modalBusy(context, phase) {
  const peer = await authenticated(await descriptor(context.session));
  const snapshot = await inspectPeer(peer);
  const operation = cameraEdit(snapshot);
  const before = (await phase("modal-open")).native;
  try {
    const rejected = await request(peer, "live.stage.set_key_value", operation);
    assert.equal(rejected.error.code, "gui_busy");
    const inspection = await request(peer, "live.inspect", { scene_epoch: snapshot.scene_epoch });
    assert.equal(inspection.error.code, "gui_busy");
    const status = (await request(peer, "live.status")).result;
    assert.equal(status.busy_observation_cached, true);
    const receipt = (await request(peer, "live.receipt", { operation_id: operation.operation_id }))
      .result;
    assert.equal(receipt.state, "rejected");
    assert.equal(receipt.error.code, "gui_busy");
  } finally {
    const after = (await phase("modal-close")).native;
    assert.deepEqual(after, before);
    await peer.close();
  }
}

const busyStates = [
  ["nested-loop-open", "nested-loop-close", "nested_event_loop"],
  ["gesture-begin", "gesture-end", "interactive_tool"],
  ["save-begin", "save-end", "saving"],
];

// A mutation arriving while the GUI is in a nested loop, a drawing gesture or
// a save is refused; its retry replays the refusal and a new edit applies once.
async function busyRefusal(peer, phase, [open, close, reason]) {
  const operation = cameraEdit(await inspectPeer(peer));
  const before = (await phase(open)).native;
  let rejected;
  try {
    rejected = await request(peer, "live.stage.set_key_value", operation);
  } finally {
    assert.deepEqual((await phase(close)).native, before, `${open} changed native state`);
  }
  assert.equal(rejected.error?.code, "gui_busy", `${open} admitted a mutation`);
  assert(rejected.error.details.reasons.includes(reason), `${open} did not report ${reason}`);
  const replayed = await request(peer, "live.stage.set_key_value", operation);
  assert.deepEqual(replayed.error, rejected.error, `${open} retry was not the retained refusal`);
  assert.deepEqual((await phase("snapshot")).native, before, `${open} retry changed state`);
  const edit = cameraEdit(await inspectPeer(peer));
  const applied = await request(peer, "live.stage.set_key_value", edit);
  assert.equal(applied.result?.changed, true, `${open} blocked a later edit`);
  const after = (await phase("snapshot")).native;
  assert.equal(after.history_index, before.history_index + 1, `${open} edit not applied once`);
  assert.deepEqual((await request(peer, "live.stage.set_key_value", edit)).result, applied.result);
  assert.deepEqual((await phase("snapshot")).native, after, `${open} edit was applied twice`);
  await phase("human-undo");
}

// Leaves an empty history, as the SDK journey expects, once the cached status
// has observed the new scene epoch that a history reset starts.
async function resetHistory(peer, phase) {
  const previous = (await request(peer, "live.status")).result.scene_epoch;
  await phase("history-reset");
  for (let attempt = 0; attempt < 40; ++attempt) {
    if ((await request(peer, "live.status")).result.scene_epoch !== previous) return;
    await delay(50);
  }
  throw new Error("The live status did not observe the history reset");
}

async function busyStatesRefuse(context, phase) {
  const peer = await authenticated(await descriptor(context.session));
  try {
    for (const state of busyStates) await busyRefusal(peer, phase, state);
    await resetHistory(peer, phase);
  } finally {
    await peer.close();
  }
  return busyStates.length;
}

function queuedRequest(method, params) {
  return { id: randomUUID(), method, params };
}

async function expiredResponses(peer, requests) {
  const expected = new Set(requests.map((item) => item.id));
  while (expected.size > 0) {
    const response = await peer.read();
    assert(expected.delete(response.id), "Unexpected or duplicate queued response");
    assert.equal(response.error.code, "deadline_exceeded");
  }
}

async function admissionDeadline(context, phase) {
  const session = await descriptor(context.session);
  const first = await authenticated(session);
  const second = await authenticated(session);
  try {
    const snapshot = await inspectPeer(first);
    const writes = Array.from({ length: 5 }, () =>
      queuedRequest("live.stage.set_key_value", cameraEdit(snapshot)),
    );
    const reads = Array.from({ length: 4 }, () =>
      queuedRequest("live.inspect", { scene_epoch: snapshot.scene_epoch }),
    );
    const before = (await phase("admission-timeout")).native;
    const started = performance.now();
    first.socket.write(Buffer.concat(writes.map(frame)));
    second.socket.write(Buffer.concat(reads.map(frame)));
    const excess = await first.read();
    assert.equal(excess.id, writes[4].id);
    assert.equal(excess.error.code, "resource_limit");
    const status = (await request(second, "live.status")).result;
    assert.equal(status.busy_observation_cached, true);
    const receipt = (
      await request(first, "live.receipt", { operation_id: writes[0].params.operation_id })
    ).result;
    assert.equal(receipt.state, "pending");
    assert(performance.now() - started < 1800, "Cached reads stalled behind the blocked GUI");
    await Promise.all([
      expiredResponses(first, writes.slice(0, 4)),
      expiredResponses(second, reads),
    ]);
    const elapsed = performance.now() - started;
    assert(elapsed >= 1500 && elapsed < 4500, "GUI admission did not expire near its 2s bound");
    assert.deepEqual(
      (await phase("snapshot")).native,
      before,
      "Expired queued edits mutated native state",
    );
    const terminal = (
      await request(first, "live.receipt", { operation_id: writes[0].params.operation_id })
    ).result;
    assert.equal(terminal.state, "rejected");
    assert.equal(terminal.error.code, "deadline_exceeded");
  } finally {
    await first.close();
    await second.close();
  }
}

async function stopAndRenew(context, phase) {
  const session = await descriptor(context.session);
  const peer = await authenticated(session);
  const before = (await phase("snapshot")).native;
  try {
    assert.deepEqual((await phase("stop")).native, before);
    await bounded(peer.finished, "Stop must revoke authenticated sockets");
    await assert.rejects(readFile(context.session), { code: "ENOENT" });
  } finally {
    await peer.close();
  }
  assert.deepEqual((await phase("restart")).native, before);
  const fresh = await descriptor(context.session);
  assert.notEqual(fresh.instance_id, session.instance_id);
  assert(fresh.token !== session.token, "A new live instance reused a previous token");
  await rejectedAuthentication(fresh, frame(authentication(session)), "Revoked previous grant");
  const current = await authenticated(fresh);
  try {
    assert((await request(current, "live.capabilities")).result);
  } finally {
    await current.close();
  }
}

/** Exercise real native session admission and local authentication boundaries.
 *
 * Args:
 *   context: Fixed GUI fixture paths for the executable, current session and evidence.
 *   phase: Fixed native-controller callback; never a product operation.
 * Returns:
 *   Redacted counts of completed boundary cases, without grant contents or tokens.
 */
export async function verifyLiveSecurity(context, phase) {
  const session = await descriptor(context.session);
  const peer = await authenticated(session);
  try {
    const result = await request(peer, "live.capabilities");
    assert(!result.error, "The real native service must work before negative cases run");
  } finally {
    await peer.close();
  }
  const report = { sessionFiles: 0 };
  await connectorBaseline(context, session, report);
  report.authentication = await authenticationFailures(session);
  if (process.platform === "win32") {
    report.windows_identities = await windowsIdentities(context, session);
    const variants = await phase("security-files");
    try {
      assert.equal(
        variants.files.length,
        5,
        "Windows security fixture must provide five ACL/reparse variants",
      );
      await connectorBaseline({ ...context, session: variants.files[0].baseline }, session);
      for (const variant of variants.files) {
        await rejectedFile(context.executable, variant.path, session.token, variant.name);
      }
      await connectorBaseline({ ...context, session: variants.files[0].baseline }, session);
      report.sessionFiles = variants.files.length;
    } finally {
      await phase("security-files-cleanup");
    }
  } else {
    report.sessionFiles = await posixFiles(context, session);
  }
  report.framing = await framingCases(session);
  await connectionLimits(session);
  await inspectMode(context, phase);
  await modalBusy(context, phase);
  report.busy_states_refused = await busyStatesRefuse(context, phase);
  await admissionDeadline(context, phase);
  await stopAndRenew(context, phase);
  report.status = "passed";
  report.connection_bounds = true;
  report.inspect_permission = true;
  report.modal_busy = true;
  report.queued_requests_expired = 8;
  report.cached_reads_during_gui_stall = true;
  report.stop_and_fresh_grant = true;
  await writeFile(
    join(context.evidence, "security-boundaries.json"),
    JSON.stringify(report) + "\n",
  );
  return report;
}
