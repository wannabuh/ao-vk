#version 450
// Fragment shader for 8-bit targets (ffp_main.glsl) with no discard: a draw that never alpha-tests or cuts out
// gets no benefit from the main shader's discard, and the discard turns off early-Z (the overdraw of the blended
// statics is not rejected). This variant keeps early-Z; the draw selects it when neither F_ALPHATEST nor F_CUTOUT
// is set (identical output).
#define RVK_NO_CUTOUT
#include "ffp_main.glsl"
