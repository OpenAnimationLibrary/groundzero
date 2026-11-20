export class WorkerError extends Error {
  constructor(
    readonly code: string,
    message: string,
    readonly details: Record<string, unknown> = {},
  ) {
    super(message);
    this.name = "WorkerError";
  }
}

/** Return a plain JSON object, rejecting arrays and null at trust boundaries. */
export function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

/** Keep native operation failures distinct from unavailable or malformed workers. */
export function errorResult(error: unknown) {
  const failure =
    error instanceof WorkerError
      ? error
      : new WorkerError("ADAPTER_ERROR", error instanceof Error ? error.message : String(error));
  return {
    isError: true,
    content: [{ type: "text" as const, text: `${failure.code}: ${failure.message}` }],
    structuredContent: {
      error: { code: failure.code, message: failure.message, details: failure.details },
    },
  };
}
