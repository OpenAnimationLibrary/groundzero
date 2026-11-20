import { Socket } from "node:net";

const mode = process.argv[2];
const channel = new Socket({ fd: 3, readable: true, writable: true });
let received = Buffer.alloc(0);
let echoed = false;
channel.on("error", () => process.exit(1));
channel.on("data", (chunk) => {
  if (echoed) {
    if (mode === "extra-after-ack") channel.write(Buffer.from([0]));
    return;
  }
  received = Buffer.concat([received, chunk]);
  if (received.length < 44) return;
  if (received.length !== 44) throw new Error("Boundary fixture received an oversized frame");
  echoed = true;
  reply(received);
});

function reply(frame) {
  switch (mode) {
    case "wrong-echo":
      frame[12] ^= 1;
      channel.write(frame);
      break;
    case "oversized-echo":
      channel.write(Buffer.concat([frame, Buffer.from([0])]));
      break;
    case "truncated-echo":
      channel.end(frame.subarray(0, 43));
      break;
    case "silent-echo":
      break;
    case "close-before-ack":
      channel.end(frame);
      break;
    case "extra-after-ack":
      channel.write(frame);
      break;
    default:
      throw new Error("Unknown native boundary fixture mode");
  }
}
