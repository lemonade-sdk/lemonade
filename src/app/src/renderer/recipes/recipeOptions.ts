import * as llamacpp from "./llamacpp/recipeOptions";
import * as whispercpp from "./whispercpp/recipeOptions";
import * as fastflow from "./fastflow/recipeOptions";
import * as onnx from "./onnx/recipeOptions";
import * as sdcpp from "./sdcpp/recipeOptions";

export interface NumericOption {
    value: number;
    useDefault: boolean;
}
export interface StringOption {
    value: string;
    useDefault: boolean;
}

export interface BooleanOption {
    value: boolean;
    useDefault: boolean;
}

export type RecipeOptions = llamacpp.LlamaOptions | whispercpp.WhisperOptions | fastflow.FlmOptions | onnx.OgaOptions | sdcpp.StableDiffusionOptions;
