export interface GridNodeData {
  id: string;
  x: number;
  y: number;
  width: number;
  prompt: string;
  text: string;
  prompts: string[];
  status: 'generating' | 'ready' | 'error';
  versionIndex: number;
  versions: NodeVersion[];
  parentId?: string;
}

export interface NodeVersion {
  prompt: string;
  text: string;
  prompts: string[];
}
