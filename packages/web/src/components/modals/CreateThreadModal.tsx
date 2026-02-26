"use client";

import React, { useState, useCallback, useEffect } from "react";
import { AnimatePresence, motion } from "framer-motion";
import {
  X,
  ChevronLeft,
  ChevronRight,
  Check,
  Server,
} from "lucide-react";
import useAppStore from "../../stores/app-store";
import { cn } from "../../lib/utils";
import type { CreateThreadRequest } from "../../types";
import { AgentWorkType } from "../../types";

// Host type enum values
enum HostType {
  Local = 0,
  Docker = 1,
  RemoteSSH = 2,
}



// SSH config interface
interface SSHConfig {
  id: string;
  alias: string;
  host: string;
  username: string;
}

interface CreateThreadModalProps {
  isOpen: boolean;
  onClose: () => void;
}

interface WizardState {
  // Step 1: Basic Info
  title: string;
  objective: string;

  // Step 2: Purpose
  purpose: string;

  // Step 3: Working Directory
  rootCwd: string;

  // Step 4: Host Configuration
  hostType: HostType;
  // Docker options
  dockerImage: string;
  dockerRepo: string;
  // SSH options
  sshMode: "existing" | "new";
  selectedSSHConfigId: string;
  sshAlias: string;
  sshHost: string;
  sshUsername: string;
  sshAuthMethod: "password" | "key";
  sshCredential: string;
}

const INITIAL_STATE: WizardState = {
  title: "",
  objective: "",
  purpose: "orchestrator",
  rootCwd: "/tmp",
  hostType: HostType.Local,
  dockerImage: "firmius-sandbox:latest",
  dockerRepo: "",
  sshMode: "existing",
  selectedSSHConfigId: "",
  sshAlias: "",
  sshHost: "",
  sshUsername: "",
  sshAuthMethod: "key",
  sshCredential: "",
};

export function CreateThreadModal({
  isOpen,
  onClose,
}: CreateThreadModalProps): React.ReactElement | null {
  const [currentStep, setCurrentStep] = useState(1);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [sshConfigs, setSSHConfigs] = useState<SSHConfig[]>([]);
  const [state, setState] = useState<WizardState>(INITIAL_STATE);

  const { createThread } = useAppStore();

  // Fetch SSH configs when opening the modal
  useEffect(() => {
    if (isOpen) {
      fetchSSHConfigs();
    }
  }, [isOpen]);



  const [isMobile, setIsMobile] = useState(false);

  useEffect(() => {
    const checkMobile = () => {
      setIsMobile(window.innerWidth < 768);
    };
    
    checkMobile();
    window.addEventListener('resize', checkMobile);
    return () => window.removeEventListener('resize', checkMobile);
  }, []);

  const fetchSSHConfigs = async () => {
    try {
      const response = await fetch("/api/ssh-configs");
      if (response.ok) {
        const data = await response.json();
        setSSHConfigs(data.configs || []);
      }
    } catch {
      // Silently fail - SSH configs are optional
      setSSHConfigs([]);
    }
  };

  const updateState = useCallback(
    <K extends keyof WizardState>(key: K, value: WizardState[K]) => {
      setState((prev) => ({ ...prev, [key]: value }));
    },
    [],
  );

  const resetAndClose = useCallback(() => {
    setCurrentStep(1);
    setState(INITIAL_STATE);
    setError(null);
    onClose();
  }, [onClose]);

  const buildHostConfig = (): Record<string, unknown> => {
    switch (state.hostType) {
      case HostType.Local:
        return { type: HostType.Local };

      case HostType.Docker:
        // Generate a unique container name
        const containerName = `firmius-${Date.now()}-${Math.random().toString(36).substring(2, 8)}`;
        return {
          type: HostType.Docker,
          options: {
            image: state.dockerImage,
            containerName,
            ...(state.dockerRepo && { repo: state.dockerRepo }),
          },
        };

      case HostType.RemoteSSH:
        if (state.sshMode === "existing" && state.selectedSSHConfigId) {
          return {
            type: HostType.RemoteSSH,
            sshConfigId: state.selectedSSHConfigId,
          };
        }
        return {
          type: HostType.RemoteSSH,
          host: state.sshHost,
          username: state.sshUsername,
          ...(state.sshAuthMethod === "password"
            ? { password: state.sshCredential }
            : { privateKey: state.sshCredential }),
        };

      default:
        return { type: HostType.Local };
    }
  };

   const buildCreateThreadRequest = (): CreateThreadRequest => {
     return {
       hostConfig: buildHostConfig(),
       rootCwd: state.rootCwd,
       purpose: state.purpose,
       objective: state.objective,
        workType: AgentWorkType.Conversational as unknown as "Conversational" | "Goal",
     };
   };

  const handleCreate = async () => {
    setIsLoading(true);
    setError(null);

    try {
      const request = buildCreateThreadRequest();
      await createThread(request);
      resetAndClose();
    } catch (err) {
      const message =
        err instanceof Error ? err.message : "Failed to create thread";
      setError(message);
    } finally {
      setIsLoading(false);
    }
  };

  // Validation for each step
  const canProceed = (): boolean => {
    switch (currentStep) {
      case 1:
        return state.rootCwd.trim().length > 0 && validateHostConfig();
      case 2:
        return true;
      default:
        return false;
    }
  };

  const validateHostConfig = (): boolean => {
    switch (state.hostType) {
      case HostType.Local:
        return true;
      case HostType.Docker:
        return state.dockerImage.trim().length > 0;
      case HostType.RemoteSSH:
        if (state.sshMode === "existing") {
          return state.selectedSSHConfigId.length > 0;
        }
        return (
          state.sshHost.trim().length > 0 &&
          state.sshUsername.trim().length > 0 &&
          state.sshCredential.trim().length > 0
        );
      default:
        return false;
    }
  };

  const handleNext = () => {
    if (currentStep < 2 && canProceed()) {
      setCurrentStep((prev) => prev + 1);
    }
  };

  const handleBack = () => {
    if (currentStep > 1) {
      setCurrentStep((prev) => prev - 1);
    }
  };

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === "Enter" && e.metaKey) {
      if (currentStep === 2) {
        void handleCreate();
      } else if (canProceed()) {
        handleNext();
      }
    }
  };

  if (!isOpen) return null;

  const steps = [
    { number: 1, title: "Host", icon: Server },
    { number: 2, title: "Review", icon: Check },
  ];

  return (
    <div
      className="fixed inset-0 z-50 flex h-full w-full flex-col items-center justify-end md:justify-center bg-black/50 md:p-4"
      onClick={(e) => {
        if (e.target === e.currentTarget) {
          resetAndClose();
        }
      }}
      onKeyDown={handleKeyDown}
    >
      <motion.div 
        initial={isMobile ? { y: "100%" } : { opacity: 0, scale: 0.95 }}
        animate={isMobile ? { y: 0 } : { opacity: 1, scale: 1 }}
        exit={isMobile ? { y: "100%" } : { opacity: 0, scale: 0.95 }}
        transition={{ type: "spring", damping: 25, stiffness: 300 }}
        className="flex w-full flex-col bg-background rounded-t-xl h-[85vh] md:h-auto md:max-h-[85vh] md:max-w-2xl md:rounded-lg md:border md:border-border md:bg-card md:shadow-xl overflow-hidden"
      >
        {/* Header */}
        <div className="flex flex-shrink-0 items-center justify-between border-b border-border px-4 py-3 md:px-6 md:py-4">
          <h2 className="text-lg font-semibold">Create New Thread</h2>
          <button
            type="button"
            onClick={resetAndClose}
            className="rounded-md p-2 text-muted-foreground hover:bg-accent hover:text-accent-foreground"
          >
            <X size={20} />
          </button>
        </div>

        {/* Progress Steps */}
        <div className="flex-shrink-0 border-b border-border px-4 py-3 md:px-6 md:py-4">
          <div className="flex items-center justify-between">
            {steps.map((step, index) => {
              const Icon = step.icon;
              const isActive = step.number === currentStep;
              const isCompleted = step.number < currentStep;

              return (
                <div key={step.number} className="flex items-center">
                  <div
                    className={cn(
                      "flex h-8 w-8 items-center justify-center rounded-full text-sm font-medium transition-colors",
                      isActive && "bg-primary text-primary-foreground",
                      isCompleted && "bg-primary/20 text-primary",
                      !isActive &&
                        !isCompleted &&
                        "bg-muted text-muted-foreground",
                    )}
                  >
                    {isCompleted ? <Check size={16} /> : <Icon size={16} />}
                  </div>
                  <span
                    className={cn(
                      "ml-2 hidden text-sm font-medium sm:block",
                      isActive && "text-foreground",
                      isCompleted && "text-primary",
                      !isActive && !isCompleted && "text-muted-foreground",
                    )}
                  >
                    {step.title}
                  </span>
                  {index < steps.length - 1 && (
                    <div
                      className={cn(
                        "mx-2 hidden h-px w-8 sm:block",
                        step.number < currentStep ? "bg-primary" : "bg-border",
                      )}
                    />
                  )}
                </div>
              );
            })}
          </div>
        </div>

        {/* Error Display */}
        {error && (
          <div className="mx-4 mt-4 flex-shrink-0 rounded-md border border-red-200 bg-red-50 p-3 text-sm text-red-800 dark:border-red-900 dark:bg-red-900/20 dark:text-red-200 md:mx-6">
            {error}
          </div>
        )}

        {/* Step Content */}
        <div className="flex-1 overflow-y-auto px-4 py-4 md:px-6 md:py-6">
          <AnimatePresence mode="wait">
            <motion.div
              key={currentStep}
              initial={{ opacity: 0, x: 20 }}
              animate={{ opacity: 1, x: 0 }}
              exit={{ opacity: 0, x: -20 }}
              transition={{ duration: 0.2 }}
              className="h-full"
            >
              {currentStep === 1 && (
                <Step1HostConfiguration
                  state={state}
                  sshConfigs={sshConfigs}
                  onUpdate={updateState}
                />
              )}
              {currentStep === 2 && <Step2Review state={state} />}
            </motion.div>
          </AnimatePresence>
        </div>

        {/* Footer */}
        <div className="flex flex-shrink-0 items-center justify-between border-t border-border bg-background p-4 md:px-6 md:py-4">
          <button
            type="button"
            onClick={currentStep === 1 ? resetAndClose : handleBack}
            className="rounded-md px-4 py-3 text-base font-medium text-muted-foreground hover:bg-accent hover:text-accent-foreground md:py-2 md:text-sm"
          >
            {currentStep === 1 ? (
              "Cancel"
            ) : (
              <>
                <ChevronLeft size={16} className="mr-1 inline" />
                Back
              </>
            )}
          </button>

          <div className="flex items-center gap-3">
            {currentStep < 2 ? (
              <button
                type="button"
                onClick={handleNext}
                disabled={!canProceed()}
                className={cn(
                  "flex items-center gap-1 rounded-md px-6 py-3 text-base font-medium md:px-4 md:py-2 md:text-sm",
                  canProceed()
                    ? "bg-primary text-primary-foreground hover:bg-primary/90"
                    : "cursor-not-allowed bg-muted text-muted-foreground",
                )}
              >
                Next
                <ChevronRight size={16} />
              </button>
            ) : (
              <button
                type="button"
                onClick={() => void handleCreate()}
                disabled={isLoading}
                className={cn(
                  "flex items-center gap-2 rounded-md px-8 py-3 text-base font-medium md:px-6 md:py-2 md:text-sm",
                  isLoading
                    ? "cursor-not-allowed bg-muted text-muted-foreground"
                    : "bg-primary text-primary-foreground hover:bg-primary/90",
                )}
              >
                {isLoading ? (
                  <>
                    <div className="h-4 w-4 animate-spin rounded-full border-2 border-current border-t-transparent" />
                    Creating...
                  </>
                ) : (
                  <>
                    <Check size={16} />
                    Create Thread
                  </>
                )}
              </button>
            )}
          </div>
        </div>

      </motion.div>
    </div>
  );
}

// Step 1: Host Configuration
interface Step1HostProps {
  state: WizardState;
  sshConfigs: SSHConfig[];
  onUpdate: <K extends keyof WizardState>(
    key: K,
    value: WizardState[K],
  ) => void;
}

function Step1HostConfiguration({ state, sshConfigs, onUpdate }: Step1HostProps) {
  const hostTypeOptions = [
    {
      value: HostType.Local,
      label: "Local",
      description: "Run on the local machine",
    },
    {
      value: HostType.Docker,
      label: "Docker",
      description: "Run in a container",
    },
    {
      value: HostType.RemoteSSH,
      label: "SSH",
      description: "Connect to a remote server",
    },
  ];

  return (
    <div className="space-y-4">
      <div>
        <label className="mb-3 block text-sm font-medium">Host Type</label>
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
          {hostTypeOptions.map((option) => (
            <button
              key={option.value}
              type="button"
              onClick={() => onUpdate("hostType", option.value)}
              className={cn(
                "rounded-md border p-3 text-left transition-colors",
                state.hostType === option.value
                  ? "border-primary bg-primary/5"
                  : "border-border hover:border-primary/50",
              )}
            >
              <div className="font-medium">{option.label}</div>
              <div className="text-xs text-muted-foreground">
                {option.description}
              </div>
            </button>
          ))}
        </div>
      </div>

      {/* Local Host Configuration */}
      {state.hostType === HostType.Local && (
        <div className="space-y-4 rounded-md border border-border p-4">
          <div>
            <label
              htmlFor="localRootCwd"
              className="mb-2 block text-sm font-medium"
            >
              Working Directory <span className="text-red-500">*</span>
            </label>
            <input
              id="localRootCwd"
              type="text"
              value={state.rootCwd}
              onChange={(e) => onUpdate("rootCwd", e.target.value)}
              placeholder="/tmp"
              className="w-full rounded-md border border-input bg-background px-3 py-2 text-base md:text-sm placeholder:text-muted-foreground focus:outline-none focus:ring-2 focus:ring-ring"
            />
            <p className="mt-2 text-sm text-muted-foreground">
              The root directory where the thread will operate. All file
              operations will be relative to this path.
            </p>
          </div>

          <div className="rounded-md border border-border bg-muted/50 p-4">
            <p className="text-sm text-muted-foreground">
              Local host uses the machine&apos;s native shell. No additional
              configuration required.
            </p>
          </div>
        </div>
      )}

      {/* Docker Host Configuration */}
      {state.hostType === HostType.Docker && (
        <div className="space-y-4 rounded-md border border-border p-4">
          <div>
            <label
              htmlFor="dockerImage"
              className="mb-2 block text-sm font-medium"
            >
              Docker Image <span className="text-red-500">*</span>
            </label>
            <input
              id="dockerImage"
              type="text"
              value={state.dockerImage}
              onChange={(e) => onUpdate("dockerImage", e.target.value)}
              placeholder="firmius-sandbox:latest"
              className="w-full rounded-md border border-input bg-background px-3 py-2 text-base md:text-sm focus:outline-none focus:ring-2 focus:ring-ring"
            />
          </div>

          <div>
            <label
              htmlFor="dockerRepo"
              className="mb-2 block text-sm font-medium"
            >
              Git Repository URL{" "}
              <span className="text-muted-foreground">(optional)</span>
            </label>
            <input
              id="dockerRepo"
              type="text"
              value={state.dockerRepo}
              onChange={(e) => onUpdate("dockerRepo", e.target.value)}
              placeholder="https://github.com/username/repo"
              className="w-full rounded-md border border-input bg-background px-3 py-2 text-base md:text-sm focus:outline-none focus:ring-2 focus:ring-ring"
            />
            <p className="mt-1 text-xs text-muted-foreground">
              Repository to clone into the container on initialization
            </p>
          </div>

          <div>
            <label
              htmlFor="dockerContainerCwd"
              className="mb-2 block text-sm font-medium"
            >
              Working Directory <span className="text-red-500">*</span>
            </label>
            <input
              id="dockerContainerCwd"
              type="text"
              value={state.rootCwd}
              onChange={(e) => onUpdate("rootCwd", e.target.value)}
              placeholder="/workspace"
              className="w-full rounded-md border border-input bg-background px-3 py-2 text-base md:text-sm placeholder:text-muted-foreground focus:outline-none focus:ring-2 focus:ring-ring"
            />
            <p className="mt-2 text-sm text-muted-foreground">
              The working directory inside the container where the thread will
              operate.
            </p>
          </div>
        </div>
      )}

      {/* SSH Host Configuration */}
      {state.hostType === HostType.RemoteSSH && (
        <div className="space-y-4 rounded-md border border-border p-4">
          {/* Working Directory */}
          <div>
            <label
              htmlFor="sshRootCwd"
              className="mb-2 block text-sm font-medium"
            >
              Remote Working Directory <span className="text-red-500">*</span>
            </label>
            <input
              id="sshRootCwd"
              type="text"
              value={state.rootCwd}
              onChange={(e) => onUpdate("rootCwd", e.target.value)}
              placeholder="/tmp"
              className="w-full rounded-md border border-input bg-background px-3 py-2 text-base md:text-sm placeholder:text-muted-foreground focus:outline-none focus:ring-2 focus:ring-ring"
            />
            <p className="mt-2 text-sm text-muted-foreground">
              The working directory on the remote host where the thread will
              operate.
            </p>
          </div>

          {/* SSH Mode Toggle */}
          <div>
            <label className="mb-2 block text-sm font-medium">
              SSH Configuration
            </label>
            <div className="flex rounded-md border border-input p-1">
              <button
                type="button"
                onClick={() => onUpdate("sshMode", "existing")}
                className={cn(
                  "flex-1 rounded-sm px-3 py-1.5 text-sm font-medium transition-colors",
                  state.sshMode === "existing"
                    ? "bg-primary text-primary-foreground"
                    : "text-muted-foreground hover:text-foreground",
                )}
              >
                Use Existing
              </button>
              <button
                type="button"
                onClick={() => onUpdate("sshMode", "new")}
                className={cn(
                  "flex-1 rounded-sm px-3 py-1.5 text-sm font-medium transition-colors",
                  state.sshMode === "new"
                    ? "bg-primary text-primary-foreground"
                    : "text-muted-foreground hover:text-foreground",
                )}
              >
                Create New
              </button>
            </div>
          </div>

          {/* Existing SSH Config */}
          {state.sshMode === "existing" && (
            <div>
              <label
                htmlFor="sshConfig"
                className="mb-2 block text-sm font-medium"
              >
                Select SSH Config <span className="text-red-500">*</span>
              </label>
              {sshConfigs.length > 0 ? (
                <select
                  id="sshConfig"
                  value={state.selectedSSHConfigId}
                  onChange={(e) =>
                    onUpdate("selectedSSHConfigId", e.target.value)
                  }
                  className="w-full rounded-md border border-input bg-background px-3 py-2 text-base md:text-sm focus:outline-none focus:ring-2 focus:ring-ring"
                >
                  <option value="">Select a configuration...</option>
                  {sshConfigs.map((config) => (
                    <option key={config.id} value={config.id}>
                      {config.alias} ({config.username}@{config.host})
                    </option>
                  ))}
                </select>
              ) : (
                <div className="rounded-md border border-dashed border-border p-4 text-center text-sm text-muted-foreground">
                  No saved SSH configurations found.
                  <br />
                  <button
                    type="button"
                    onClick={() => onUpdate("sshMode", "new")}
                    className="mt-2 text-primary hover:underline"
                  >
                    Create a new configuration
                  </button>
                </div>
              )}
            </div>
          )}

          {/* New SSH Config */}
          {state.sshMode === "new" && (
            <div className="space-y-4">
              <div>
                <label
                  htmlFor="sshAlias"
                  className="mb-2 block text-sm font-medium"
                >
                  Host Alias{" "}
                  <span className="text-muted-foreground">(optional)</span>
                </label>
                <input
                  id="sshAlias"
                  type="text"
                  value={state.sshAlias}
                  onChange={(e) => onUpdate("sshAlias", e.target.value)}
                  placeholder="My Server"
                  className="w-full rounded-md border border-input bg-background px-3 py-2 text-base md:text-sm focus:outline-none focus:ring-2 focus:ring-ring"
                />
              </div>

              <div>
                <label
                  htmlFor="sshHost"
                  className="mb-2 block text-sm font-medium"
                >
                  Hostname/IP <span className="text-red-500">*</span>
                </label>
                <input
                  id="sshHost"
                  type="text"
                  value={state.sshHost}
                  onChange={(e) => onUpdate("sshHost", e.target.value)}
                  placeholder="example.com or 192.168.1.1"
                  className="w-full rounded-md border border-input bg-background px-3 py-2 text-base md:text-sm focus:outline-none focus:ring-2 focus:ring-ring"
                />
              </div>

              <div>
                <label
                  htmlFor="sshUsername"
                  className="mb-2 block text-sm font-medium"
                >
                  Username <span className="text-red-500">*</span>
                </label>
                <input
                  id="sshUsername"
                  type="text"
                  value={state.sshUsername}
                  onChange={(e) => onUpdate("sshUsername", e.target.value)}
                  placeholder="root"
                  className="w-full rounded-md border border-input bg-background px-3 py-2 text-base md:text-sm focus:outline-none focus:ring-2 focus:ring-ring"
                />
              </div>

              <div>
                <label className="mb-2 block text-sm font-medium">
                  Authentication Method
                </label>
                <div className="flex gap-4">
                  <label className="flex items-center gap-2">
                    <input
                      type="radio"
                      name="sshAuthMethod"
                      value="key"
                      checked={state.sshAuthMethod === "key"}
                      onChange={(e) =>
                        onUpdate(
                          "sshAuthMethod",
                          e.target.value as "key" | "password",
                        )
                      }
                      className="h-4 w-4"
                    />
                    <span className="text-sm">SSH Key</span>
                  </label>
                  <label className="flex items-center gap-2">
                    <input
                      type="radio"
                      name="sshAuthMethod"
                      value="password"
                      checked={state.sshAuthMethod === "password"}
                      onChange={(e) =>
                        onUpdate(
                          "sshAuthMethod",
                          e.target.value as "key" | "password",
                        )
                      }
                      className="h-4 w-4"
                    />
                    <span className="text-sm">Password</span>
                  </label>
                </div>
              </div>

              <div>
                <label
                  htmlFor="sshCredential"
                  className="mb-2 block text-sm font-medium"
                >
                  {state.sshAuthMethod === "key" ? "Private Key" : "Password"}{" "}
                  <span className="text-red-500">*</span>
                </label>
                {state.sshAuthMethod === "key" ? (
                  <textarea
                    id="sshCredential"
                    value={state.sshCredential}
                    onChange={(e) => onUpdate("sshCredential", e.target.value)}
                    placeholder="-----BEGIN OPENSSH PRIVATE KEY-----&#10;...&#10;-----END OPENSSH PRIVATE KEY-----"
                    rows={4}
                    className="w-full resize-none rounded-md border border-input bg-background px-3 py-2 text-sm font-mono text-xs focus:outline-none focus:ring-2 focus:ring-ring"
                  />
                ) : (
                  <input
                    id="sshCredential"
                    type="password"
                    value={state.sshCredential}
                    onChange={(e) => onUpdate("sshCredential", e.target.value)}
                    placeholder="Enter password"
                    className="w-full rounded-md border border-input bg-background px-3 py-2 text-base md:text-sm focus:outline-none focus:ring-2 focus:ring-ring"
                  />
                )}
              </div>
            </div>
          )}
        </div>
      )}
    </div>
  );
}

// Step 2: Review
interface Step2ReviewProps {
  state: WizardState;
}

function Step2Review({ state }: Step2ReviewProps) {
  const hostTypeLabels: Record<HostType, string> = {
    [HostType.Local]: "Local",
    [HostType.Docker]: "Docker",
    [HostType.RemoteSSH]: "SSH",
  };

  const getHostDetails = (): string => {
    switch (state.hostType) {
      case HostType.Local:
        return "Local machine shell";
      case HostType.Docker:
        return `${state.dockerImage}${state.dockerRepo ? ` (cloning ${state.dockerRepo})` : ""}`;
      case HostType.RemoteSSH:
        return state.sshMode === "existing"
          ? "Using existing SSH configuration"
          : `${state.sshUsername}@${state.sshHost}`;
      default:
        return "";
    }
  };

  return (
    <div className="space-y-4">
      <p className="text-sm text-muted-foreground">
        Review your thread configuration before creating it.
      </p>

      <div className="space-y-3 rounded-md border border-border p-4">
        <ReviewItem label="Purpose" value="Orchestrator (Top-level agent)" />
        <ReviewItem label="Working Directory" value={state.rootCwd} />
        <ReviewItem label="Host Type" value={hostTypeLabels[state.hostType]} />
        <ReviewItem label="Host Details" value={getHostDetails()} />
      </div>

      <div className="rounded-md border border-blue-200 bg-blue-50 p-4 dark:border-blue-900 dark:bg-blue-900/20">
        <p className="text-sm text-blue-800 dark:text-blue-200">
          <strong>Tip:</strong> Press{" "}
          <kbd className="rounded bg-blue-100 px-1.5 py-0.5 font-mono text-xs dark:bg-blue-800">
            Cmd+Enter
          </kbd>{" "}
          to create the thread immediately.
        </p>
      </div>
    </div>
  );
}

function ReviewItem({
  label,
  value,
  multiline = false,
}: {
  label: string;
  value: string;
  multiline?: boolean;
}) {
  return (
    <div className="flex flex-col gap-1 sm:flex-row sm:gap-4">
      <dt className="min-w-[140px] text-sm font-medium text-muted-foreground">
        {label}
      </dt>
      <dd className={cn("flex-1 text-sm", multiline && "whitespace-pre-wrap")}>
        {value}
      </dd>
    </div>
  );
}

export default CreateThreadModal;
