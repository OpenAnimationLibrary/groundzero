import { expect, test } from "vitest";
import { decodeLiveJson, liveRequest } from "#mcp/live-protocol";

test("counts request limits in serialized UTF-8 bytes and refuses non-finite values", () => {
  expect(() => liveRequest("request", "live.inspect", { text: "界".repeat(22000) })).toThrow(
    "64 KiB",
  );
  expect(() => liveRequest("request", "live.inspect", { value: Number.POSITIVE_INFINITY })).toThrow(
    "non-finite",
  );
});

test("rejects malformed UTF-8 and excessive response nesting", () => {
  expect(() => decodeLiveJson(Buffer.from([0xff]))).toThrow();
  let nested: unknown = {};
  for (let index = 0; index < 18; index += 1) nested = { nested };
  expect(() => decodeLiveJson(Buffer.from(JSON.stringify(nested)))).toThrow("nesting");
  expect(() => decodeLiveJson(Buffer.from('{"number":1e999}'))).toThrow("non-finite");
});

test("writes the UTF-8 body byte count in the four-byte big-endian header", () => {
  const frame = liveRequest("identity", "live.receipt", { operation_id: "operation" });
  expect(frame.readUInt32BE(0)).toBe(frame.length - 4);
  expect(JSON.parse(frame.subarray(4).toString("utf8"))).toEqual({
    id: "identity",
    method: "live.receipt",
    params: { operation_id: "operation" },
  });
});
