import type { Tool } from "@modelcontextprotocol/server";
import { outputSchema, sessionEffects } from "#mcp/declarations";
import { WorkerError, isObject } from "#mcp/errors";

/** Most native tools one session may expose at once. */
export const MAX_EXPOSED_TOOLS = 256;
const MAX_TOOLSETS = 64;
const TOOLSET_NAME = /^[a-z][a-z0-9-]{0,31}$/;
const REQUIRED_TOOLSET = "core";

export interface ToolsetDescriptor {
  name: string;
  description: string;
  tools: string[];
}

const namesSchema = {
  type: "object" as const,
  additionalProperties: false,
  required: ["names"],
  properties: {
    names: {
      type: "array",
      minItems: 1,
      maxItems: MAX_TOOLSETS,
      uniqueItems: true,
      items: { type: "string", pattern: TOOLSET_NAME.source },
    },
  },
};

const listing = {
  toolsets: {
    type: "array",
    minItems: 1,
    maxItems: MAX_TOOLSETS,
    items: {
      type: "object",
      additionalProperties: false,
      required: ["name", "description", "tool_count", "enabled", "granted"],
      properties: {
        name: { type: "string", minLength: 1, maxLength: 32 },
        description: { type: "string", minLength: 1, maxLength: 1024 },
        tool_count: { type: "integer", minimum: 0, maximum: 1024 },
        enabled: { type: "boolean" },
        granted: { type: "boolean" },
      },
    },
  },
  exposed_tool_count: { type: "integer", minimum: 0, maximum: MAX_EXPOSED_TOOLS },
};

function listingSchema(changes: boolean) {
  const properties = changes ? { ...listing, changed: { type: "boolean" } } : listing;
  return outputSchema({
    type: "object",
    additionalProperties: false,
    required: Object.keys(properties),
    properties,
  });
}

const LOAD_ERRORS = [
  "INVALID_ARGUMENTS",
  "WORKER_UNAVAILABLE",
  "INVALID_CATALOG",
  "INCOMPATIBLE_WORKER",
  "RESULT_TOO_LARGE",
];

interface ToolsetToolSpec {
  name: string;
  description: string;
  changes: boolean;
  errors: string[];
}

const DECLARATIONS = new Map<string, Record<string, unknown>>();

function toolsetTool({ name, description, changes, errors }: ToolsetToolSpec): Tool {
  DECLARATIONS.set(name, {
    name,
    toolset: "adapter",
    effects: sessionEffects(changes ? "session" : "read"),
    errors: [...LOAD_ERRORS, ...errors],
    outputSchema: listingSchema(changes),
  });
  return {
    name,
    description,
    inputSchema: changes
      ? namesSchema
      : { type: "object", additionalProperties: false, properties: {} },
    outputSchema: listingSchema(changes),
    annotations: {
      readOnlyHint: !changes,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  };
}

/** The declaration `tool.describe` returns for one of the adapter's toolset tools. */
export function toolsetDeclaration(name: string): Record<string, unknown> | undefined {
  return DECLARATIONS.get(name);
}

/** Adapter-owned tools that switch which native toolsets this session exposes. */
export const TOOLSET_TOOLS: Tool[] = [
  toolsetTool({
    name: "toolsets.list",
    description:
      "List the native toolsets, what each covers, how many tools it has, whether this " +
      "session's launch grants it and whether it is enabled.",
    changes: false,
    errors: [],
  }),
  toolsetTool({
    name: "toolsets.enable",
    description:
      "Expose the tools of the named toolsets in this session. The tool list then changes. " +
      "Only toolsets the launch grants (--domains) can be enabled.",
    changes: true,
    errors: ["UNKNOWN_TOOLSET", "TOOLSET_LIMIT", "GRANT_REQUIRED"],
  }),
  toolsetTool({
    name: "toolsets.disable",
    description:
      "Hide the tools of the named toolsets in this session. The core toolset stays enabled.",
    changes: true,
    errors: ["UNKNOWN_TOOLSET", "TOOLSET_REQUIRED"],
  }),
];

const TOOLSET_TOOL_NAMES = new Set(TOOLSET_TOOLS.map((tool) => tool.name));

function invalidCatalog(message: string): WorkerError {
  return new WorkerError("INVALID_CATALOG", message);
}

function isToolsetName(value: unknown): value is string {
  return typeof value === "string" && TOOLSET_NAME.test(value);
}

function isDescription(value: unknown): value is string {
  return typeof value === "string" && value.length > 0 && value.length <= 1024;
}

function parseDescriptor(entry: unknown): ToolsetDescriptor {
  if (
    !isObject(entry) ||
    !isToolsetName(entry["name"]) ||
    !isDescription(entry["description"]) ||
    !Array.isArray(entry["tools"])
  ) {
    throw invalidCatalog("Invalid native toolset descriptor");
  }
  const tools = entry["tools"];
  if (!tools.every((tool): tool is string => typeof tool === "string")) {
    throw invalidCatalog("Invalid native toolset descriptor");
  }
  return { name: entry["name"], description: entry["description"], tools };
}

function owners(toolsets: ToolsetDescriptor[], catalog: Set<string>): Map<string, string> {
  const result = new Map<string, string>();
  for (const toolset of toolsets) {
    for (const tool of toolset.tools) {
      if (!catalog.has(tool) || result.has(tool)) {
        throw invalidCatalog("Native toolsets must partition the tool catalog");
      }
      result.set(tool, toolset.name);
    }
  }
  if (result.size !== catalog.size) {
    throw invalidCatalog("Native toolsets must partition the tool catalog");
  }
  return result;
}

/** Validate that the native toolsets partition the catalog and include core.
 *
 * Args:
 *   value: The `toolsets` field of the native capabilities.
 *   tools: The validated native tools.
 * Returns:
 *   The toolsets in native order.
 */
export function parseToolsets(value: unknown, tools: Tool[]): ToolsetDescriptor[] {
  if (!Array.isArray(value) || value.length === 0 || value.length > MAX_TOOLSETS) {
    throw invalidCatalog(`Expected 1 to ${MAX_TOOLSETS} native toolsets`);
  }
  const catalog = new Set(tools.map((tool) => tool.name));
  const reserved = [...catalog].find((name) => TOOLSET_TOOL_NAMES.has(name));
  if (reserved !== undefined) throw invalidCatalog(`Native tool ${reserved} is reserved`);
  const toolsets = value.map(parseDescriptor);
  const names = new Set(toolsets.map((toolset) => toolset.name));
  if (names.size !== toolsets.length) throw invalidCatalog("Duplicate native toolset name");
  if (!names.has(REQUIRED_TOOLSET)) throw invalidCatalog("The native catalog has no core toolset");
  owners(toolsets, catalog);
  return toolsets;
}

/** Which native toolsets one MCP session exposes. */
export class ToolsetState {
  private readonly enabled = new Set<string>([REQUIRED_TOOLSET]);
  private readonly owners = new Map<string, string>();

  /**
   * Args:
   *   toolsets: The native toolsets.
   *   tools: The validated native tools.
   *   requested: Toolsets enabled at start; "all" enables every granted toolset.
   *   domains: The toolsets the launch grants, or ["all"].
   */
  constructor(
    private readonly toolsets: ToolsetDescriptor[],
    private readonly tools: Tool[],
    requested: string[],
    private readonly domains: string[] = ["all"],
  ) {
    for (const toolset of toolsets) {
      for (const tool of toolset.tools) this.owners.set(tool, toolset.name);
    }
    const names = requested.includes("all")
      ? toolsets.map((toolset) => toolset.name).filter((name) => this.isGranted(name))
      : requested;
    this.enable(names);
  }

  /** Every native tool plus the toolset tools, for argument validation. */
  all(): Tool[] {
    return [...this.tools, ...TOOLSET_TOOLS];
  }

  /** Native tools of enabled toolsets in catalog order, then the toolset tools. */
  visible(): Tool[] {
    return [...this.tools.filter((tool) => this.isEnabled(tool.name)), ...TOOLSET_TOOLS];
  }

  /** Report this session's toolset states in a runtime.inspect result.
   *
   * The worker serves every toolset; only the adapter knows which ones this session exposes.
   * A toolset is configured, and usable, when it is enabled here.
   *
   * Args:
   *   result: The native runtime.inspect result.
   * Returns:
   *   The result with each toolset state's configured, usable and reasons set for this session.
   */
  sessionRuntime(result: unknown): unknown {
    if (!isObject(result) || !Array.isArray(result["toolsets"])) return result;
    const toolsets = result["toolsets"].map((entry: unknown) => {
      if (!isObject(entry) || typeof entry["name"] !== "string" || !isObject(entry["state"])) {
        return entry;
      }
      const enabled = this.enabled.has(entry["name"]);
      const state = entry["state"];
      const reasons = Array.isArray(state["reasons"])
        ? state["reasons"].filter((reason) => reason !== "toolset_disabled")
        : [];
      return {
        ...entry,
        state: {
          ...state,
          configured: enabled,
          usable: state["usable"] === true && enabled,
          reasons: enabled ? reasons : [...reasons, "toolset_disabled"],
        },
      };
    });
    return { ...result, toolsets };
  }

  isToolsetTool(name: string): boolean {
    return TOOLSET_TOOL_NAMES.has(name);
  }

  /** Reject a native tool whose toolset this session has not enabled. */
  requireEnabled(name: string): void {
    const owner = this.owners.get(name);
    if (owner === undefined || this.enabled.has(owner)) return;
    throw new WorkerError("TOOLSET_DISABLED", `Enable the "${owner}" toolset to use ${name}`, {
      toolset: owner,
    });
  }

  list(): Record<string, unknown> {
    return {
      toolsets: this.toolsets.map((toolset) => ({
        name: toolset.name,
        description: toolset.description,
        tool_count: toolset.tools.length,
        enabled: this.enabled.has(toolset.name),
        granted: this.isGranted(toolset.name),
      })),
      exposed_tool_count: this.exposedCount(this.enabled),
    };
  }

  /** Enable toolsets; returns whether the exposed tools changed. */
  enable(names: string[]): boolean {
    this.requireKnown(names);
    const ungranted = names.filter((name) => !this.isGranted(name));
    if (ungranted.length > 0) {
      throw new WorkerError(
        "GRANT_REQUIRED",
        `This session's launch does not grant ${ungranted.join(", ")}; restart the adapter ` +
          "with them in --domains",
        { toolsets: ungranted },
      );
    }
    const next = new Set([...this.enabled, ...names]);
    const exposed = this.exposedCount(next);
    if (exposed > MAX_EXPOSED_TOOLS) {
      throw new WorkerError(
        "TOOLSET_LIMIT",
        `Enabling these toolsets would expose ${exposed} tools; the limit is ${MAX_EXPOSED_TOOLS}`,
        { exposed_tool_count: exposed, limit: MAX_EXPOSED_TOOLS },
      );
    }
    return this.replace(next);
  }

  /** Disable toolsets other than core; returns whether the exposed tools changed. */
  disable(names: string[]): boolean {
    this.requireKnown(names);
    if (names.includes(REQUIRED_TOOLSET)) {
      throw new WorkerError("TOOLSET_REQUIRED", "The core toolset cannot be disabled");
    }
    return this.replace(new Set([...this.enabled].filter((name) => !names.includes(name))));
  }

  private isGranted(toolset: string): boolean {
    return (
      toolset === REQUIRED_TOOLSET || this.domains.includes("all") || this.domains.includes(toolset)
    );
  }

  private isEnabled(tool: string): boolean {
    const owner = this.owners.get(tool);
    return owner !== undefined && this.enabled.has(owner);
  }

  private exposedCount(enabled: Set<string>): number {
    return this.toolsets
      .filter((toolset) => enabled.has(toolset.name))
      .reduce((total, toolset) => total + toolset.tools.length, 0);
  }

  private requireKnown(names: string[]): void {
    const known = new Set(this.toolsets.map((toolset) => toolset.name));
    const unknown = names.filter((name) => !known.has(name));
    if (unknown.length > 0) {
      throw new WorkerError("UNKNOWN_TOOLSET", `Unknown toolset: ${unknown.join(", ")}`, {
        unknown,
        available: [...known],
      });
    }
  }

  private replace(next: Set<string>): boolean {
    const changed =
      next.size !== this.enabled.size || [...next].some((name) => !this.enabled.has(name));
    this.enabled.clear();
    for (const name of next) this.enabled.add(name);
    return changed;
  }
}
