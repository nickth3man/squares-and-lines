// Canvas domain logic — node model, spatial layout, versioning, tree operations.
// This is the backend-owned domain layer that the frontend calls via REST.
// Ported identically to every language backend.

import { createProvider } from "./providers";

// ---------------------------------------------------------------------------
// Types (mirrors frontend/src/types.ts — now backend-owned)
// ---------------------------------------------------------------------------

export interface InlineLink {
  label: string;
  target: string;
}

export interface NodeVersion {
  prompt: string;
  text: string;
  prompts: string[];
  links: InlineLink[];
}

export interface GridNodeData {
  id: string;
  x: number;
  y: number;
  width: number;
  height?: number;
  prompt: string;
  text: string;
  prompts: string[];
  links: InlineLink[];
  status: "generating" | "ready" | "error";
  versionIndex: number;
  versions: NodeVersion[];
  parentId?: string;
}

export interface Canvas {
  id: string;
  nodes: GridNodeData[];
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const NODE_WIDTH = 400;

// ---------------------------------------------------------------------------
// Canvas store (in-memory, per-session)
// ---------------------------------------------------------------------------

const canvases = new Map<string, Canvas>();

export function createCanvas(): Canvas {
  const id = `canvas-${Date.now()}-${Math.floor(Math.random() * 100000)}`;
  const canvas: Canvas = { id, nodes: [] };
  canvases.set(id, canvas);
  return canvas;
}

export function getCanvas(id: string): Canvas | undefined {
  return canvases.get(id);
}

// ---------------------------------------------------------------------------
// Node ID generation
// ---------------------------------------------------------------------------

function generateNodeId(): string {
  return `node-${Date.now()}-${Math.floor(Math.random() * 1000)}`;
}

// ---------------------------------------------------------------------------
// Spatial layout — collision-aware child placement
// ---------------------------------------------------------------------------

function computeChildPosition(parent: GridNodeData, nodes: GridNodeData[]): { x: number; y: number } {
  const newX = parent.x + parent.width + 400;
  const initialOffset = Math.random() > 0.5 ? 200 : -200;
  let newY = parent.y + initialOffset;
  const cardHeight = parent.height || 400;

  let isOccupied = true;
  let offsetMultiplier = 1;
  let direction = Math.random() > 0.5 ? 1 : -1;

  while (isOccupied) {
    isOccupied = nodes.some((n) => {
      const nH = n.height || 400;
      const xOverlap = Math.abs(n.x + NODE_WIDTH / 2 - (newX + NODE_WIDTH / 2)) < NODE_WIDTH;
      const yOverlap = Math.abs(n.y + nH / 2 - (newY + cardHeight / 2)) < (nH + cardHeight) / 2;
      return xOverlap && yOverlap;
    });

    if (isOccupied) {
      newY = parent.y + initialOffset + cardHeight * offsetMultiplier * direction;
      direction *= -1;
      if (direction === 1) offsetMultiplier++;
    }
  }

  return { x: newX, y: newY };
}

// ---------------------------------------------------------------------------
// Node operations
// ---------------------------------------------------------------------------

/** Create a root or child node, call the LLM, and return the complete node. */
export async function generateNode(
  canvas: Canvas,
  prompt: string,
  parentId?: string,
): Promise<GridNodeData> {
  const id = generateNodeId();
  let x = 0;
  let y = 0;
  if (parentId) {
    const parent = canvas.nodes.find((n) => n.id === parentId);
    if (!parent) throw new Error(`Parent node ${parentId} not found`);
    const pos = computeChildPosition(parent, canvas.nodes);
    x = pos.x;
    y = pos.y;
  }

  const node: GridNodeData = {
    id,
    x,
    y,
    width: NODE_WIDTH,
    prompt,
    text: "",
    prompts: [],
    links: [],
    status: "generating",
    versionIndex: 0,
    versions: [],
    parentId,
  };
  canvas.nodes.push(node);

  try {
    const result = await createProvider().generateText(prompt);
    node.text = result.text || "No text";
    node.prompts = result.prompts;
    node.links = result.links;
    node.status = "ready";
    node.versions = [{ prompt, text: node.text, prompts: node.prompts, links: node.links }];
  } catch (error) {
    console.error(error);
    node.status = "error";
  }

  return node;
}

/** Regenerate a node — creates a new version with a fresh LLM call. */
export async function regenerateNode(canvas: Canvas, nodeId: string): Promise<GridNodeData> {
  const node = canvas.nodes.find((n) => n.id === nodeId);
  if (!node) throw new Error(`Node ${nodeId} not found`);

  node.status = "generating";
  try {
    const result = await createProvider().generateText(node.prompt);
    const newVersion: NodeVersion = {
      prompt: node.prompt,
      text: result.text || "No text",
      prompts: result.prompts,
      links: result.links,
    };
    node.versions.push(newVersion);
    node.versionIndex = node.versions.length - 1;
    node.text = newVersion.text;
    node.prompts = newVersion.prompts;
    node.links = newVersion.links;
    node.status = "ready";
  } catch (error) {
    console.error(error);
    node.status = "error";
  }

  return node;
}

/** Delete a node and all its descendants. Returns all deleted IDs. */
export function deleteNode(canvas: Canvas, nodeId: string): string[] {
  const getDescendants = (id: string): string[] => {
    const children = canvas.nodes.filter((n) => n.parentId === id).map((n) => n.id);
    let descendants = [...children];
    for (const childId of children) descendants = [...descendants, ...getDescendants(childId)];
    return descendants;
  };

  const toDelete = [nodeId, ...getDescendants(nodeId)];
  const deleteSet = new Set(toDelete);
  canvas.nodes = canvas.nodes.filter((n) => !deleteSet.has(n.id));
  return toDelete;
}

/** Switch the active version of a node. */
export function setNodeVersion(canvas: Canvas, nodeId: string, versionIndex: number): GridNodeData {
  const node = canvas.nodes.find((n) => n.id === nodeId);
  if (!node) throw new Error(`Node ${nodeId} not found`);
  node.versionIndex = versionIndex;
  const version = node.versions[versionIndex];
  if (version) {
    node.text = version.text;
    node.prompts = version.prompts;
    node.links = version.links;
  }
  return node;
}

/** Persist a node's user-adjusted canvas position. */
export function setNodePosition(canvas: Canvas, nodeId: string, x: number, y: number): GridNodeData {
  const node = canvas.nodes.find((n) => n.id === nodeId);
  if (!node) throw new Error(`Node ${nodeId} not found`);
  node.x = x;
  node.y = y;
  return node;
}

/** Update a node's measured height (from frontend rendering). */
export function measureNode(canvas: Canvas, nodeId: string, height: number): void {
  const node = canvas.nodes.find((n) => n.id === nodeId);
  if (node) node.height = height;
}

/** Serialize a node for JSON response (strips nothing — all fields are public). */
export function serializeNode(node: GridNodeData): GridNodeData {
  return { ...node };
}
