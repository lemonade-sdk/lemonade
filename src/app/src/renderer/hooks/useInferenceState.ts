import { useState, useRef, useCallback } from 'react';
import { ensureModelReady, DownloadAbortError } from '../utils/backendInstaller';
import { ModelsData } from '../utils/modelData';

export type InferencePhase = 'idle' | 'pre_flight' | 'inferring';
export type Modality = 'llm' | 'embedding' | 'reranking' | 'transcription' | 'image' | 'speech';

interface RunPreFlightOptions {
  modelName: string;
  modelsData: ModelsData;
  onError: (msg: string) => void;
}

export function useInferenceState() {
  const [phase, setPhase] = useState<InferencePhase>('idle');
  const [activeModality, setActiveModality] = useState<Modality | null>(null);
  const phaseRef = useRef<InferencePhase>('idle');

  const isIdle = phase === 'idle';
  const isPreFlight = phase === 'pre_flight';
  const isInferring = phase === 'inferring';
  const isBusy = phase !== 'idle';

  const setPhaseSync = useCallback((p: InferencePhase) => {
    phaseRef.current = p;
    setPhase(p);
  }, []);

  const runPreFlight = useCallback(async (
    modality: Modality,
    { modelName, modelsData, onError }: RunPreFlightOptions,
  ): Promise<boolean> => {
    // Re-entry guard using synchronous ref
    if (phaseRef.current !== 'idle') return false;

    setPhaseSync('pre_flight');
    setActiveModality(modality);

    try {
      await ensureModelReady(modelName, modelsData, {
        onModelLoading: () => {}, // Phase already set to pre_flight
      });
    } catch (error: any) {
      setPhaseSync('idle');
      setActiveModality(null);
      if (error instanceof DownloadAbortError) return false;
      onError(error.message || 'Failed to prepare model for inference.');
      return false;
    }

    setPhaseSync('inferring');
    return true;
  }, [setPhaseSync]);

  const reset = useCallback(() => {
    setPhaseSync('idle');
    setActiveModality(null);
  }, [setPhaseSync]);

  return {
    phase,
    activeModality,
    phaseRef,
    isIdle,
    isPreFlight,
    isInferring,
    isBusy,
    runPreFlight,
    reset,
  };
}
