import React, { useState, useRef, useCallback } from 'react';
import { Search, Play, Sparkles, ZoomIn, ZoomOut } from 'lucide-react';
import { GridNodeData } from './types';
import { NodeCard } from './components/NodeCard';
import { ConnectingLines } from './components/ConnectingLines';
import { Minimap } from './components/Minimap';

const NODE_WIDTH = 400;

export default function App() {
  const [nodes, setNodes] = useState<GridNodeData[]>([]);
  const [searchQuery, setSearchQuery] = useState('');
  
  // Canvas Transform State
  const [transform, setTransform] = useState({ 
    x: 0, 
    y: 0, 
    scale: typeof window !== 'undefined' && window.innerWidth < 768 ? 0.6 : 0.85 
  });
  const [isDragging, setIsDragging] = useState(false);
  const dragStartInfo = useRef({ x: 0, y: 0, transformX: 0, transformY: 0 });
  const containerRef = useRef<HTMLDivElement>(null);
  
  // Node dragging state
  const [draggingNodeId, setDraggingNodeId] = useState<string | null>(null);
  const nodeDragStartInfo = useRef({ x: 0, y: 0, nodeX: 0, nodeY: 0 });

  // Always-current nodes ref so callbacks can be stable (useCallback []).
  const nodesRef = useRef(nodes);
  nodesRef.current = nodes;

  // Panning Support
  const handlePointerDown = (e: React.PointerEvent) => {
    // Only pan on left click background
    if (e.target === containerRef.current || (e.target as HTMLElement).classList.contains('canvas-bg')) {
      e.currentTarget.setPointerCapture(e.pointerId);
      setIsDragging(true);
      dragStartInfo.current = {
        x: e.clientX,
        y: e.clientY,
        transformX: transform.x,
        transformY: transform.y,
      };
      // prevent selection while dragging
      e.preventDefault();
    }
  };

  const handleNodeDragStart = useCallback((e: React.PointerEvent, nodeId: string) => {
    e.stopPropagation();
    // Capture pointer on the element being grabbed so fast drags don't break
    e.currentTarget.setPointerCapture(e.pointerId);
    const node = nodesRef.current.find(n => n.id === nodeId);
    if (!node) return;
    setDraggingNodeId(nodeId);
    nodeDragStartInfo.current = {
      x: e.clientX,
      y: e.clientY,
      nodeX: node.x,
      nodeY: node.y,
    };
  }, []);

  const handlePointerMove = (e: React.PointerEvent) => {
    if (isDragging) {
      const dx = e.clientX - dragStartInfo.current.x;
      const dy = e.clientY - dragStartInfo.current.y;
      setTransform(prev => ({
        ...prev,
        x: dragStartInfo.current.transformX + dx,
        y: dragStartInfo.current.transformY + dy,
      }));
    } else if (draggingNodeId) {
      const dx = (e.clientX - nodeDragStartInfo.current.x) / transform.scale;
      const dy = (e.clientY - nodeDragStartInfo.current.y) / transform.scale;
      setNodes(prev => prev.map(n => 
        n.id === draggingNodeId ? { ...n, x: nodeDragStartInfo.current.nodeX + dx, y: nodeDragStartInfo.current.nodeY + dy } : n
      ));
    }
  };

  const handlePointerUp = (e: React.PointerEvent) => {
    setIsDragging(false);
    setDraggingNodeId(null);
    if ((e.target as HTMLElement).hasPointerCapture && (e.target as HTMLElement).hasPointerCapture(e.pointerId)) {
      (e.target as HTMLElement).releasePointerCapture(e.pointerId);
    }
  };

  const handleWheel = (e: React.WheelEvent) => {
    // Ctrl/Meta+wheel zooms; plain wheel pans
    if (e.ctrlKey || e.metaKey) {
      e.preventDefault();
      const zoomFactor = -e.deltaY * 0.002;
      handleZoom(zoomFactor, e.clientX, e.clientY);
    } else {
      setTransform(prev => ({
        ...prev,
        x: prev.x - e.deltaX,
        y: prev.y - e.deltaY,
      }));
    }
  };

  const handleZoom = (delta: number, originX?: number, originY?: number) => {
    setTransform(prev => {
      const newScale = Math.min(Math.max(0.2, prev.scale + delta), 3);
      
      if (originX === undefined || originY === undefined) {
        // Zoom to center if no origin given
        originX = window.innerWidth / 2;
        originY = window.innerHeight / 2;
      }

      // Adjust x and y to keep the zoom centered on the pointer
      const scaleRatio = newScale / prev.scale;
      const newX = originX - (originX - prev.x) * scaleRatio;
      const newY = originY - (originY - prev.y) * scaleRatio;

      return { x: newX, y: newY, scale: newScale };
    });
  };

  const centerOnPosition = useCallback((x: number, y: number) => {
    const hw = window.innerWidth / 2;
    const hh = window.innerHeight / 2;
    setTransform(prev => ({
      ...prev,
      x: hw - x * prev.scale - (NODE_WIDTH / 2) * prev.scale,
      y: hh - y * prev.scale - 200 * prev.scale
    }));
  }, []);

  const handleCenterViewport = useCallback((x: number, y: number) => {
    const hw = window.innerWidth / 2;
    const hh = window.innerHeight / 2;
    setTransform(prev => ({
      ...prev,
      x: hw - x * prev.scale,
      y: hh - y * prev.scale
    }));
  }, []);

  const generateNode = useCallback(async (prompt: string, x: number, y: number, parentId?: string, isRegenerationOf?: string) => {
    const id = isRegenerationOf || `node-${Date.now()}-${Math.floor(Math.random() * 1000)}`;
    
    // Create new node or update existing to 'generating'
    if (isRegenerationOf) {
      setNodes(prev => prev.map(n => n.id === id ? { ...n, status: 'generating' } : n));
    } else {
      const newNode: GridNodeData = {
        id, x, y,
        width: NODE_WIDTH,
        prompt,
        text: '',
        prompts: [],
        status: 'generating',
        versionIndex: 0,
        versions: [],
        parentId
      };
      setNodes(prev => [...prev, newNode]);
    }
    
    // Center view locally
    setTimeout(() => centerOnPosition(x, y), 50);

    try {
      // 1. Fetch text
      const res = await fetch('/api/generate', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ prompt }),
      });
      const data = await res.json();
      
      if (data.error) throw new Error(data.error);

      setNodes(prev => prev.map(node => {
        if (node.id === id) {
          const newVersion = {
            prompt,
            text: data.text || 'No text',
            prompts: data.prompts || [],
          };
          
          return {
            ...node,
            status: 'ready',
            versions: isRegenerationOf ? [...node.versions, newVersion] : [newVersion],
            versionIndex: isRegenerationOf ? node.versions.length : 0,
            text: newVersion.text,
            prompts: newVersion.prompts,
          };
        }
        return node;
      }));

    } catch (error) {
      console.error(error);
      setNodes(prev => prev.map(node => node.id === id ? { ...node, status: 'error' } : node));
    }
  }, [centerOnPosition]);

  const handleSearchSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    if (!searchQuery.trim()) return;

    generateNode(searchQuery, 0, 0);
    setSearchQuery('');
  };

  const handleMagicWand = () => {
    const ideas = ["Evolution of Video Games", "Quantum Entanglement", "Rise of the Roman Empire", "History of Artificial Intelligence", "Deep Sea Ecosystems"];
    generateNode(ideas[Math.floor(Math.random() * ideas.length)], 0, 0);
  };

  const handleExpand = useCallback((prompt: string, parentId: string) => {
    const parent = nodesRef.current.find(n => n.id === parentId);
    if (!parent) return;
    
    // Spawn to the right
    const newX = parent.x + parent.width + 400; // 400px gap for prompts and connections
    
    // Add initial offset so the line is never perfectly straight
    const initialOffset = Math.random() > 0.5 ? 200 : -200;
    let newY = parent.y + initialOffset;
    const cardWidth = parent.width;
    const cardHeight = parent.height || 400;
    
    let isOccupied = true;
    let offsetMultiplier = 1;
    let direction = Math.random() > 0.5 ? 1 : -1; // 1 for down, -1 for up
    
    // AABB overlap detection using measured card dimensions
    while (isOccupied) {
      isOccupied = nodesRef.current.some(n => {
        const nH = n.height || 400;
        const xOverlap = Math.abs((n.x + cardWidth / 2) - (newX + cardWidth / 2)) < cardWidth;
        const yOverlap = Math.abs((n.y + nH / 2) - (newY + cardHeight / 2)) < (nH + cardHeight) / 2;
        return xOverlap && yOverlap;
      });
      
      if (isOccupied) {
        newY = parent.y + initialOffset + (cardHeight * offsetMultiplier * direction);
        direction *= -1;
        if (direction === 1) {
          offsetMultiplier++;
        }
      }
    }
    
    generateNode(prompt, newX, newY, parentId);
  }, [generateNode]);

  const handleRegenerate = useCallback((nodeId: string) => {
    const node = nodesRef.current.find(n => n.id === nodeId);
    if (node) {
      generateNode(node.prompt, node.x, node.y, node.parentId, nodeId);
    }
  }, [generateNode]);

  const handleDelete = useCallback((nodeId: string) => {
    // Delete this node and all descendants
    const getDescendants = (id: string, allNodes: GridNodeData[]): string[] => {
      const children = allNodes.filter(n => n.parentId === id).map(n => n.id);
      let desc = [...children];
      for (const childId of children) {
        desc = [...desc, ...getDescendants(childId, allNodes)];
      }
      return desc;
    };
    
    const toDelete = [nodeId, ...getDescendants(nodeId, nodesRef.current)];
    setNodes(prev => prev.filter(n => !toDelete.includes(n.id)));
  }, []);

  const setVersion = useCallback((nodeId: string, versionIndex: number) => {
    setNodes(prev => prev.map(n => n.id === nodeId ? { ...n, versionIndex } : n));
  }, []);

  const handleMeasure = useCallback((nodeId: string, height: number) => {
    setNodes(prev => prev.map(n => (n.id === nodeId && n.height !== height) ? { ...n, height } : n));
  }, []);
  
  const totalChars = nodes.reduce((acc, node) => acc + ((node.versions[node.versionIndex]?.text?.length) || 0), 0);

  return (
    <div 
      className="relative w-screen h-screen overflow-hidden dot-grid canvas-bg"
      ref={containerRef}
      onPointerDown={handlePointerDown}
      onPointerMove={handlePointerMove}
      onPointerUp={handlePointerUp}
      onPointerLeave={handlePointerUp}
      onWheel={handleWheel}
      style={{ cursor: isDragging ? 'grabbing' : 'grab' }}
    >
      {/* Draggable/Zoomable Canvas */}
      <div 
        className="absolute top-0 left-0 origin-top-left canvas-bg"
        style={{
          transform: `translate(${transform.x}px, ${transform.y}px) scale(${transform.scale})`,
          width: '100%',
          height: '100%'
        }}
      >
        <ConnectingLines nodes={nodes} />
        {nodes.map((node, index) => (
          <NodeCard 
            key={node.id} 
            index={index}
            node={node} 
            onExpand={handleExpand}
            onRegenerate={handleRegenerate}
            onClose={handleDelete}
            setVersion={setVersion}
            onNodeDragStart={handleNodeDragStart}
            onMeasure={handleMeasure}
          />
        ))}
      </div>

      {/* Initial Search UI */}
      {nodes.length === 0 && (
        <div className="absolute inset-0 flex flex-col items-center justify-center pointer-events-none p-4">
          <h1 
            className="font-tomorrow font-bold text-4xl md:text-[60px] tracking-[-0.02em] mb-4 md:mb-2 text-black pointer-events-auto"
          >
            GRIDSCAPE
          </h1>
          <div className="pointer-events-auto w-full max-w-2xl bg-white border-2 border-black p-2 md:p-2 shadow-[4px_4px_0px_#000] md:shadow-[8px_8px_0px_#000] flex flex-col sm:flex-row gap-2 sm:gap-0">
            <form onSubmit={handleSearchSubmit} className="flex-1 flex px-2 sm:px-4 items-center">
              <Search className="text-gray-400 mr-2 sm:mr-3 shrink-0" size={20} />
              <input
                autoFocus
                className="flex-1 outline-none font-sans text-base sm:text-lg lg:text-xl py-2 sm:py-3 w-full"
                placeholder="What do you want to explore?"
                value={searchQuery}
                onChange={(e) => setSearchQuery(e.target.value)}
              />
            </form>
            <div className="flex gap-2 w-full sm:w-auto justify-end">
              <button 
                onClick={handleSearchSubmit}
                className="flex-1 sm:flex-none sm:w-12 h-12 bg-black text-white flex items-center justify-center hover:bg-gray-800 transition-colors cursor-pointer"
                aria-label="Explore topic"
              >
                <Play fill="currentColor" size={20} />
              </button>
              <button 
                onClick={handleMagicWand}
                className="flex-1 sm:flex-none sm:w-12 h-12 border-2 border-black text-black bg-yellow-100 flex items-center justify-center hover:bg-yellow-200 transition-colors cursor-pointer"
                aria-label="Surprise me with a random topic"
              >
                <Sparkles size={20} />
              </button>
            </div>
          </div>
          <div className="mt-6 md:mt-8 text-center text-[10px] md:text-xs font-mono text-gray-500 max-w-lg leading-relaxed uppercase tracking-widest px-4 md:px-0">
            GRIDSCAPE IS AN INFINITE SPATIAL-KNOWLEDGE-ENGINE FOR MAPPING COMPLEX IDEAS... START WITH ANY TOPIC AND GENERATE NON-LINEAR RESEARCH NODES...
          </div>
        </div>
      )}

      {/* Floating UI Elements */}
      
      {nodes.length > 0 && (
        <>
          {/* Capacity Badge */}
          <div className="absolute bottom-6 left-1/2 -translate-x-1/2 pointer-events-none md:bottom-6 md:left-6 md:translate-x-0 hidden md:block">
            <div className="bg-black text-white px-3 py-1.5 md:px-4 md:py-2 rounded-full font-mono text-[8px] md:text-[10px] uppercase tracking-widest shadow-lg">
              CAPACITY: {nodes.length} NODES
            </div>
          </div>

          {/* Tools Menu & Minimap */}
          <div className="absolute bottom-4 right-4 md:bottom-6 md:right-6 pointer-events-auto flex flex-col items-end gap-2 md:gap-4">
            <div className="hidden md:block">
              <Minimap
                nodes={nodes}
                transform={transform}
                onCenterViewport={handleCenterViewport}
              />
            </div>
            
            <div className="bg-black text-white p-1 rounded-xl flex items-center shadow-2xl">
              <div className="hidden sm:block px-4 font-mono text-[10px] min-w-[100px] text-center border-r border-gray-700">
                {totalChars} CHARS
              </div>
              
              <button 
                onClick={() => handleZoom(-0.2)}
                className="w-8 h-8 md:w-10 md:h-10 flex items-center justify-center hover:bg-gray-800 rounded-lg transition-colors mx-1 cursor-pointer"
                aria-label="Zoom out"
              >
                <ZoomOut size={16} className="md:w-[18px] md:h-[18px]" />
              </button>
              
              <button 
                onClick={() => handleZoom(0.2)}
                className="w-8 h-8 md:w-10 md:h-10 flex items-center justify-center hover:bg-gray-800 rounded-lg transition-colors mr-1 cursor-pointer"
                aria-label="Zoom in"
              >
                <ZoomIn size={16} className="md:w-[18px] md:h-[18px]" />
              </button>
            </div>
          </div>
        </>
      )}
{/* Support returning back to zero state search button */}
      {nodes.length > 0 && (
      <div className="absolute top-4 left-4 md:top-6 md:left-6">
            <button 
                onClick={() => {
                  if (window.confirm('Reset the entire canvas? All nodes will be lost.')) {
                    setNodes([]);
                    setTransform({ 
                      x: 0, 
                      y: 0, 
                      scale: typeof window !== 'undefined' && window.innerWidth < 768 ? 0.6 : 0.85 
                    });
                  }
                }}
                className="bg-white border-2 border-black text-[10px] md:text-xs font-mono font-bold px-3 py-2 md:px-4 md:py-2 hover:bg-red-50 hover:text-red-600 transition-colors shadow-[2px_2px_0px_#000] md:shadow-[4px_4px_0px_#000] cursor-pointer cursor-auto"
            >
                [ RESET CANVAS ]
            </button>
      </div>
      )}
    </div>
  );
}
