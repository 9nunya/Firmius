'use client';

import { useEffect, useState } from 'react';
import useAppStore from '@/stores/app-store';
import { client } from '@firmius/shared/api';
import { Button } from '@/components/ui/button';
import { ModelSelector } from '@/components/ui/model-selector';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { cn } from '@/lib/utils';

export default function SettingsPanel() {
  const { 
    userConfig, 
    fetchUserConfig, 
    updateUserConfig, 
    providers, 
    loadProviders, 
    threads, 
    activeThreadId 
  } = useAppStore();

  const [draft, setDraft] = useState<{ 
    defaultModels: Record<string, { 
      providerId: string; 
      modelId: string; 
      reasoningEffort?: string 
    }> 
  }>(() => ({
    defaultModels: {},
  }));
  
  const [saving, setSaving] = useState(false);
  const [message, setMessage] = useState<{ type: 'success' | 'error'; text: string } | null>(null);
  const [purposes, setPurposes] = useState<string[]>([]);
  const [purposesLoading, setPurposesLoading] = useState(true);

  useEffect(() => {
    if (!userConfig) {
      void fetchUserConfig();
    } else {
      // Ensure we convert UserConfig to the shape draft expects if it's an array or different structure
      const defaultModels: Record<string, any> = {};
      if (Array.isArray(userConfig.defaultModels)) {
        userConfig.defaultModels.forEach((m: any) => {
          defaultModels[m.purpose] = { 
            providerId: m.providerId, 
            modelId: m.modelId, 
            reasoningEffort: m.reasoningEffort 
          };
        });
      } else {
        Object.assign(defaultModels, userConfig.defaultModels);
      }
      setDraft({ defaultModels });
    }
  }, [userConfig, fetchUserConfig]);

  useEffect(() => {
    if (providers.length === 0) {
      void loadProviders();
    }
  }, [providers.length, loadProviders]);

  useEffect(() => {
    const fetchPurposes = async () => {
      try {
        const data = await client.getPurposes();
        setPurposes(data);
      } catch (error) {
        console.error('Failed to fetch purposes:', error);
        setPurposes(["orchestrator", "architect", "coder", "mapper", "executor", "verifier"]);
      } finally {
        setPurposesLoading(false);
      }
    };
    
    if (purposes.length === 0 && purposesLoading) {
      void fetchPurposes();
    }
  }, []);

  const activeThread = threads.find(t => t.id === activeThreadId);

  const handleSave = async () => {
    setSaving(true);
    setMessage(null);
    
    try {
      // Construct the UserConfig back to the expected backend format (likely with array of objects)
      const formattedConfig: any = { ...userConfig };
      if (Array.isArray(userConfig?.defaultModels)) {
        formattedConfig.defaultModels = Object.entries(draft.defaultModels).map(([purpose, config]) => ({
          purpose,
          ...config
        }));
      } else {
        formattedConfig.defaultModels = draft.defaultModels;
      }

      await updateUserConfig(formattedConfig);
      setMessage({ type: 'success', text: 'Settings saved successfully' });
      
      // Clear success message after 3 seconds
      setTimeout(() => setMessage(null), 3000);
    } catch (e: any) {
      setMessage({ type: 'error', text: 'Failed to save settings' });
    } finally {
      setSaving(false);
    }
  };

  if (!userConfig) {
    return (
      <div className="p-6">
        <div className="animate-pulse text-muted-foreground">Loading settings...</div>
      </div>
    );
  }

  const uniquePurposes = Array.from(new Set(purposes)).sort();

  return (
    <div className="p-6 space-y-8 max-w-3xl mx-auto">
      <div className="border-b border-border pb-4">
        <h2 className="text-xl font-semibold">Settings</h2>
        <p className="text-sm text-muted-foreground mt-1">
          Configure default models and preferences
        </p>
      </div>

      {/* Current Thread Info */}
      {activeThread && (
        <section className="space-y-3">
          <h3 className="text-sm font-medium text-muted-foreground uppercase tracking-wider">
            Current Thread
          </h3>
          <div className="bg-muted/50 rounded-sm p-4 space-y-2">
            <div className="flex justify-between items-center">
              <span className="text-sm">Thread ID</span>
              <span className="text-sm font-mono text-muted-foreground">
                {activeThread.id.slice(0, 8)}...
              </span>
            </div>
            <div className="flex justify-between items-center">
              <span className="text-sm">Provider</span>
              <span className="text-sm font-medium">{activeThread.providerId}</span>
            </div>
            <div className="flex justify-between items-center">
              <span className="text-sm">Model</span>
              <span className="text-sm font-medium">{activeThread.modelId}</span>
            </div>
            {activeThread.tokensLimit && (
              <div className="flex justify-between items-center">
                <span className="text-sm">Context Limit</span>
                <span className="text-sm font-medium">
                  {(activeThread.tokensLimit / 1000).toFixed(0)}k tokens
                </span>
              </div>
            )}
          </div>
        </section>
      )}

      {/* Default Models by Purpose */}
      <section className="space-y-4">
        <h3 className="text-sm font-medium text-muted-foreground uppercase tracking-wider">
          Default Models by Purpose
        </h3>
        
        <div className="space-y-4">
          {uniquePurposes.map((purpose: string) => {
            const current = draft.defaultModels[purpose] || { 
              providerId: '', 
              modelId: '', 
              reasoningEffort: undefined 
            };
            
            const selectedProvider = providers.find(p => p.id === current.providerId);
            const selectedModel = selectedProvider?.models.find(m => m.id === current.modelId);
            const supportsReasoning = selectedModel?.capabilities?.reasoning;

            return (
              <div 
                key={purpose} 
                className="bg-muted/30 rounded-sm p-4 space-y-3"
              >
                <div className="flex items-center justify-between">
                  <span className="font-medium capitalize">{purpose}</span>
                </div>
                
                <div className="space-y-3">
                  <ModelSelector
                    providers={providers}
                    value={{ 
                      providerId: current.providerId, 
                      modelId: current.modelId 
                    }}
                    onChange={(providerId, modelId) => {
                      setDraft(prev => ({
                        defaultModels: {
                          ...prev.defaultModels,
                          [purpose]: { 
                            providerId, 
                            modelId, 
                            reasoningEffort: undefined 
                          },
                        },
                      }));
                    }}
                  />
                  
                  {supportsReasoning && (
                    <div className="flex items-center gap-3">
                      <span className="text-sm text-muted-foreground w-24">
                        Reasoning
                      </span>
                      <Select
                        value={current.reasoningEffort || 'none'}
                        onValueChange={(reasoningEffort: string) => {
                          setDraft(prev => ({
                            defaultModels: {
                              ...prev.defaultModels,
                              [purpose]: { 
                                ...(prev.defaultModels[purpose] || { 
                                  providerId: '', 
                                  modelId: '' 
                                }), 
                                reasoningEffort 
                              },
                            },
                          }));
                        }}
                      >
                        <SelectTrigger className="flex-1">
                          <SelectValue placeholder="Select effort" />
                        </SelectTrigger>
                        <SelectContent>
                          <SelectItem value="none">None</SelectItem>
                          <SelectItem value="minimal">Minimal</SelectItem>
                          <SelectItem value="low">Low</SelectItem>
                          <SelectItem value="medium">Medium</SelectItem>
                          <SelectItem value="high">High</SelectItem>
                        </SelectContent>
                      </Select>
                    </div>
                  )}
                </div>
              </div>
            );
          })}
        </div>
      </section>

      {/* Save Button */}
      <div className="flex items-center gap-4 pt-4 border-t border-border">
        <Button 
          onClick={handleSave} 
          disabled={saving}
          className="min-w-[120px]"
        >
          {saving ? 'Saving...' : 'Save Settings'}
        </Button>
        
        {message && (
          <span className={cn(
            "text-sm",
            message.type === 'success' ? 'text-green-500' : 'text-red-500'
          )}>
            {message.text}
          </span>
        )}
      </div>
    </div>
  );
}
