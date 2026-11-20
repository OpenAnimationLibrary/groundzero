import { Socket } from "node:net";

/** Act as the native boundary for adapter-only process-tree fixtures. */
export async function acceptControlChannel() {
  const marker = process.env.GROUNDZERO_AUTOMATION_CONTROL_FD;
  if (marker === undefined) return;
  if (marker !== "3") throw new Error("Fixture received an invalid private control descriptor");
  const channel = new Socket({ fd: 3, readable: true, writable: true });
  let stage = "frame";
  let received = Buffer.alloc(0);
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("Fixture handshake timed out")), 5_000);
    channel.on("data", (chunk) => {
      received = Buffer.concat([received, chunk]);
      if (stage === "frame") {
        if (received.length < 44) return;
        if (!validFrame(received)) {
          reject(new Error("Fixture received an invalid native handshake"));
          return;
        }
        channel.write(received);
        received = Buffer.alloc(0);
        stage = "ack";
        return;
      }
      if (stage === "ack" && received.length === 1 && received[0] === 1) {
        stage = "ready";
        received = Buffer.alloc(0);
        clearTimeout(timer);
        channel.unref();
        resolve();
        return;
      }
      clearTimeout(timer);
      reject(new Error("Fixture private channel received unexpected bytes"));
      process.exitCode = 1;
      channel.destroy();
    });
    channel.once("error", (error) => {
      clearTimeout(timer);
      reject(error);
    });
    channel.once("end", () => {
      clearTimeout(timer);
      if (stage !== "ready") reject(new Error("Fixture control channel ended during startup"));
      process.exit(1);
    });
  });
}

function validFrame(frame) {
  return (
    frame.length === 44 &&
    frame.readUInt32LE(0) === 0x475a4153 &&
    frame.readUInt32LE(4) === 1 &&
    frame.readUInt32LE(8) === process.ppid
  );
}
