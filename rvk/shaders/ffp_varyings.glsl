// The vertex shader's outputs (ffp.vert), as the tessellation stages pass them on: X(type, name, location).
#define FFP_VARYINGS(X) \
    X(vec4, vDiffuse, 0) X(vec4, vSpecular, 1) X(vec4, vTex0, 2) X(vec4, vTex1, 3) X(float, vFogDist, 4) \
    X(float, vFogFactor, 5) X(vec3, vMatAmbient, 6) X(vec3, vMatEmissive, 7) X(vec3, vPosW, 8) X(vec4, vNormalW, 9) \
    X(vec2, vSet0, 10) X(vec4, vClip, 11) X(vec4, vPrevClip, 12) X(float, vCutout, 13) X(vec3, vSmoothN, 14)
