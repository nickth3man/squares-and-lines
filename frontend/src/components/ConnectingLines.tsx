import React, { useMemo } from 'react';
import { GridNodeData } from '../types';

interface ConnectingLinesProps {
  nodes: GridNodeData[];
}

export const ConnectingLines: React.FC<ConnectingLinesProps> = React.memo(({ nodes }) => {
  const paths = useMemo(() => {
    const byId = new Map(nodes.map(n => [n.id, n]));
    return nodes
      .filter(n => n.parentId)
      .map(node => {
        const parent = byId.get(node.parentId!);
        if (!parent) return null;

        const parentH = parent.height || 400;
        const childH = node.height || 400;

        // Parent connection point: right side, vertical center
        const startX = parent.x + parent.width;
        const startY = parent.y + parentH / 2;

        // Child connection point: left side, vertical center
        const endX = node.x;
        const endY = node.y + childH / 2;

        // Horizontal sweeping bezier curve
        const cp1X = startX + (endX - startX) / 2;
        const cp2X = cp1X;

        return (
          <path
            key={`line-${node.id}`}
            d={`M ${startX} ${startY} C ${cp1X} ${startY}, ${cp2X} ${endY}, ${endX} ${endY}`}
            fill="none"
            stroke="black"
            strokeWidth="2"
            strokeDasharray="6 4"
          />
        );
      })
      .filter(Boolean);
  }, [nodes]);

  return (
    <svg
      style={{
        position: 'absolute',
        top: 0,
        left: 0,
        width: '100%',
        height: '100%',
        pointerEvents: 'none',
        zIndex: 0,
        overflow: 'visible'
      }}
    >
      {paths}
    </svg>
  );
});
