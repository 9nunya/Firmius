/** @jsxImportSource @opentui/react */
import { useMemo, useState, useEffect } from 'react';
import { type BoxProps } from '@opentui/react';
import { interpolateColors } from '../../utils/colors.ts';

export interface StatusGradientBarProps extends Omit<BoxProps, 'width' | 'height'> {
  pulse?: boolean;
  width?: number | 'auto' | `${number}%`;
  height?: number | 'auto' | `${number}%`;
  tokensUsed?: number;
  tokensLimit?: number;
  modelName?: string;
  providerId?: string;
}

/**
 * StatusGradientBar component for the bottom bar.
 */
export function StatusGradientBar({
  pulse = false,
  width = '100%',
  height = 1,
  tokensUsed = 0,
  tokensLimit = 0,
  modelName = 'Unknown Model',
  providerId = 'unknown',
  ...props
}: StatusGradientBarProps) {
  const [offset, setOffset] = useState(0);

  useEffect(() => {
    if (!pulse) {
      setOffset(0);
      return;
    }
    const interval = setInterval(() => {
      setOffset((prev) => (prev + 0.05) % 1);
    }, 150);
    return () => clearInterval(interval);
  }, [pulse]);

  const baseColors = useMemo(
    () => ['#000000', '#111111', '#222222', '#111111', '#000000'],
    []
  );
  
  const backgroundColor = useMemo(() => {
    return interpolateColors(baseColors, (0.5 + offset) % 1);
  }, [offset, baseColors]);

  const status = pulse ? 'WORKING' : 'IDLE';
  const tokenStr = tokensLimit > 0 ? `${tokensUsed}/${tokensLimit}` : `${tokensUsed}`;

  return (
    <box
      {...props}
      height={height}
      width={width}
      backgroundColor={backgroundColor}
      flexDirection="row"
      alignItems="center"
      paddingLeft={1}
      paddingRight={1}
      justifyContent="space-between"
    >
      <box flexDirection="row">
        <text fg="#888888">FIRMIUS </text>
        <text fg={pulse ? '#00FF00' : '#444444'}>● </text>
        <text fg="#555555">STATUS: {status}</text>
      </box>

      <box flexDirection="row">
        <text fg="#666666">{providerId.toUpperCase()} / {modelName} </text>
        <text fg="#888888">| CTX: {tokenStr}</text>
      </box>
    </box>
  );
}
