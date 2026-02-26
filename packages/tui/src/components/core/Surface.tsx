/** @jsxImportSource @opentui/react */
import { type BoxProps } from '@opentui/react';

export interface SurfaceProps extends Omit<BoxProps, 'width' | 'height'> {
  bg?: string;
  padding?: number;
  width?: number | 'auto' | `${number}%`;
  height?: number | 'auto' | `${number}%`;
  children?: React.ReactNode;
}

/**
 * Surface component providing a solid background block with padding.
 * Adheres to Cyber-Brutalist Void: obsidian background, no borders.
 */
export function Surface({
  bg = '#0A0A0A',
  padding = 1,
  width,
  height,
  children,
  ...props
}: SurfaceProps) {
  return (
    <box
      {...props}
      backgroundColor={bg}
      padding={padding}
      width={width}
      height={height}
      border={false}
    >
      {children}
    </box>
  );
}
