import { createInterface } from "node:readline";

const mode = process.argv[2];

function writeInvalidResponse(request) {
  switch (mode) {
    case "malformed":
      process.stdout.write("native diagnostic on stdout\n");
      break;
    case "utf8":
      process.stdout.write(Buffer.from([123, 34, 120, 34, 58, 34, 255, 34, 125, 10]));
      break;
    case "oversized":
      process.stdout.write("x".repeat(8 * 1024 * 1024 + 1));
      break;
    case "unknown":
      process.stdout.write('{"id":"unknown","result":{}}\n');
      break;
    case "ambiguous":
      process.stdout.write(JSON.stringify({ id: request.id, result: {}, error: {} }) + "\n");
      break;
    default:
      return false;
  }
  return true;
}

function writeFixtureResponse(request) {
  const response = JSON.stringify({ id: request.id, result: { value: request.params } });
  switch (mode) {
    case "split": {
      const bytes = Buffer.from(`${response}\n`);
      for (let offset = 0; offset < bytes.length; offset += 1) {
        process.stdout.write(bytes.subarray(offset, offset + 1));
      }
      return;
    }
    case "error":
      process.stdout.write(
        JSON.stringify({
          id: request.id,
          error: { code: "REVISION_CONFLICT", message: "Scene changed", details: { revision: 7 } },
        }) + "\n",
      );
      return;
    case "exit":
      process.exitCode = 3;
      process.stdin.destroy();
      return;
    case "final":
      process.stdout.write(response + "\n");
      process.stdin.destroy();
      return;
    case "stall":
      return;
    default:
      process.stdout.write(response + "\n");
  }
}

createInterface({ input: process.stdin }).on("line", (line) => {
  const request = JSON.parse(line);
  if (!writeInvalidResponse(request)) writeFixtureResponse(request);
});
