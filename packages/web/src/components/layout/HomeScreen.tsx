"use client";

import React from "react";
import { cn } from "../../lib/utils";
import { Plus, MessageSquare, Sparkles } from "lucide-react";

interface HomeScreenProps {
  onCreateThread?: () => void;
  className?: string;
}

export function HomeScreen({
  onCreateThread,
  className,
}: HomeScreenProps): React.ReactElement {
  return (
    <div
      className={cn(
        "flex flex-col items-center justify-center h-full px-6 py-12 w-full max-w-3xl mx-auto",
        className,
      )}
    >
      {/* Logo */}
      <div className="relative mb-6 md:mb-8">
        {/* Light mode logo */}
        <img
          src="/logo-both-text-black.webp"
          alt="Firmius"
          className="h-18 md:h-18 w-auto dark:hidden"
        />
        {/* Dark mode logo */}
        <img
          src="/logo-both-text-white.webp"
          alt="Firmius"
          className="h-18 md:h-18 w-auto hidden dark:block"
        />
      </div>

      <p className="text-sm md:text-lg text-muted-foreground text-center max-w-md mb-4 md:mb-8 px-4">
        Your AI-powered development assistant. Create a thread to start coding,
        researching, or delegating tasks to specialized agents.
      </p>

      {/* Features */}
      <div className="grid grid-cols-2 gap-2 md:gap-4 w-full max-w-2xl mb-6 md:mb-10 px-4 md:px-0">
        <FeatureCard
          icon={<Sparkles className="h-5 w-5" />}
          title="AI Agents"
          description="Spawn specialized subagents for complex tasks"
        />
        <FeatureCard
          icon={<MessageSquare className="h-5 w-5" />}
          title="Real-time Chat"
          description="Streamlined conversation with your AI assistant"
        />
      </div>

      {/* CTA Button */}
      <button
        type="button"
        onClick={onCreateThread}
        className={cn(
          "flex items-center gap-2 px-6 md:px-8 py-3 md:py-4 text-sm md:text-base font-semibold rounded-xl",
          "bg-primary text-primary-foreground hover:bg-primary/90",
          "shadow-lg hover:shadow-xl transition-all duration-200",
          "border border-primary/20",
        )}
      >
        <Plus size={18} className="md:w-5 md:h-5" />
        <span>Create New Thread</span>
      </button>

      {/* Footer hint */}
      <p className="mt-4 md:mt-8 text-xs md:text-sm text-muted-foreground text-center">
        Or select an existing thread from the sidebar
      </p>
    </div>
  );
}

interface FeatureCardProps {
  icon: React.ReactNode;
  title: string;
  description: string;
}

function FeatureCard({
  icon,
  title,
  description,
}: FeatureCardProps): React.ReactElement {
  return (
    <div className="flex flex-col items-center text-center p-4 rounded-xl bg-card border border-border hover:border-primary/30 hover:bg-accent/50 transition-colors">
      <div className="flex items-center justify-center w-10 h-10 rounded-lg bg-primary/10 text-primary mb-3">
        {icon}
      </div>
      <h3 className="font-semibold text-foreground mb-1">{title}</h3>
      <p className="text-xs text-muted-foreground">{description}</p>
    </div>
  );
}

export default HomeScreen;
