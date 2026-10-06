#version 450
// The depth pre-pass's fragment shader for cut-out draws (prepass.cpp): depth only where the draw's pixel ends up
// fully opaque - an alpha-tested draw where its alpha test passes, a blended cut-out (F_CUTOUT) where its alpha is 1.
// Everything behind such a pixel is hidden in the final image whatever order it is drawn in, so the main pass can
// reject it early; partly transparent pixels write nothing here and blend over what is behind them as before.
#define RVK_PREPASS_CUTOUT
#include "ffp_main.glsl"
