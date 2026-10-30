#pragma once

// For headers that name the facade without calling it: model_manager.h pulls the whole
// models layer into everything that includes it.
//
// The declarations live here once because ModelInfo is a struct, and a hand-typed
// `class ModelInfo;` is an MSVC warning at every use.

namespace lemon {

class ModelManager;
struct ModelInfo;

}  // namespace lemon
