import React from 'react';
import { type TextProps } from '@opentui/react';
import { interpolateColors } from '../../utils/colors.ts';

export interface GradientTextProps extends Omit<TextProps, 'children'> {
  text: string;
  colors: string[];
}

/**
 * GradientText component that renders text with a linear gradient effect.
 * Interpolates colors character-by-character.
 */
export const GradientText: React.FC<GradientTextProps> = ({
  text,
  colors,
  ...props
}) => {
  if (colors.length === 0) return <text {...props}>{text}</text>;
  if (colors.length === 1) return <text {...props} fg={colors[0]}>{text}</text>;

  return (
    <text {...props}>
      {text.split('').map((char, i) => {
        const factor = text.length > 1 ? i / (text.length - 1) : 0;
        const color = interpolateColors(colors, factor);
        return (
          <span key={i} fg={color}>
            {char}
          </span>
        );
      })}
    </text>
  );
};
