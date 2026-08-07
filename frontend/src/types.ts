export interface InlineLink {
  label: string;
  target: string;
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
  status: 'generating' | 'ready' | 'error';
  versionIndex: number;
  versions: NodeVersion[];
  parentId?: string;
}

export interface NodeVersion {
  prompt: string;
  text: string;
  prompts: string[];
  links: InlineLink[];
}
