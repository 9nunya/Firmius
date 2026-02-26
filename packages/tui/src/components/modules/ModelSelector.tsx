/** @jsxImportSource @opentui/react */
import { useState, useEffect, useMemo } from "react";
import { useKeyboard } from "@opentui/react";
import useAppStore from "../../store/appStore";

/**
 * ModelSelector: Modal for switching models in the current thread.
 */
export function ModelSelector() {
  const activeModal = useAppStore((state) => state.activeModal);
  const setActiveModal = useAppStore((state) => state.setActiveModal);
  const providers = useAppStore((state) => state.providers);
  const updateThreadSettings = useAppStore((state) => state.updateThreadSettings);

  const [selectedIndex, setSelectedIndex] = useState(0);

  const models = useMemo(() => {
    return providers.flatMap(p => p.models.map(m => ({ ...m, providerId: p.id })));
  }, [providers]);

  useEffect(() => {
    if (activeModal === "models") {
      setSelectedIndex(0);
    }
  }, [activeModal]);

  useKeyboard((key) => {
    if (activeModal !== "models") return;
    if (key.name === "escape") {
      setActiveModal(null);
      return;
    }
    if (key.name === "up") {
      setSelectedIndex((prev) => (prev > 0 ? prev - 1 : Math.max(0, models.length - 1)));
    } else if (key.name === "down") {
      setSelectedIndex((prev) => (prev < models.length - 1 ? prev + 1 : 0));
    } else if (key.name === "enter" || key.name === "return") {
      const selected = models[selectedIndex];
      if (selected) {
        updateThreadSettings({
          modelId: selected.id,
          providerId: (selected as any).providerId
        });
        setActiveModal(null);
      }
    }
  });

  if (activeModal !== "models") return null;

  const modalWidth = 60;
  const modalHeight = 15;

  return (
    <box
      position="absolute"
      top="50%"
      left="50%"
      marginTop={-7}
      marginLeft={-30}
      width={modalWidth}
      height={modalHeight}
      zIndex={20}
      backgroundColor="#0A0A0A"
    >
      <box paddingX={1} paddingY={1} backgroundColor="#1A1A1A" flexDirection="row" justifyContent="space-between">
        <text fg="#00FF00"><b>MODEL SELECTOR</b></text>
        <text fg="#444444">ESC TO EXIT // ENTER TO SELECT</text>
      </box>

      <box flexGrow={1} padding={1} flexDirection="column">
        {models.length === 0 ? (
          <box flexGrow={1} justifyContent="center" alignItems="center">
            <text fg="#666666">No models available.</text>
          </box>
        ) : (
          models.map((model: any, idx: number) => {
            const isActive = idx === selectedIndex;
            return (
              <box
                key={`${model.providerId}-${model.id}`}
                paddingX={1}
                backgroundColor={isActive ? "#333333" : "transparent"}
                flexDirection="row"
                justifyContent="space-between"
              >
                <text fg={isActive ? "#00FF00" : "#EEEEEE"}>
                  {`${isActive ? "► " : "  "}${model.name}`}
                </text>
                <text fg="#666666">{`(${model.providerId})`}</text>
              </box>
            );
          })
        )}
      </box>
    </box>
  );
}
