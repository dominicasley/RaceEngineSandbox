#version 450
// The driver's mirror: the glass of the centre mirror and both door mirrors, showing the rear-facing
// mirror camera's view and nothing else.
//
// **One picture serves all three surfaces, and the asset says which part each shows.** Assetto
// Corsa renders one wide rear view into a 2:1 texture and lets each mirror surface address it by
// its own UVs — the modder places every mirror's UV island on `mirror_placement.png`, whose left
// third is labelled for the right door mirror, whose right third is for the left, and whose middle
// is the centre mirror (the labels are written backwards, because the texture is the *camera's*
// picture and a mirror shows a camera's picture reversed). This engine keeps that contract exactly:
// the map bound at MIRROR_MAP_BINDING is the plain, unreversed picture of a camera at the driver's
// eye looking straight back along the car, and the reversal is the asset's own UV winding —
// measured on the Golf, u increases towards the car's left on all three surfaces, which is where
// an unreversed rear view puts the car's left. Nothing here flips anything.
//
// The UVs are wrapped rather than trusted: the KN5 stores v negated, so the surfaces address
// (-0.93, -0.07) where the map is (0, 1), and the attachment sampler clamps. `fract` is what the
// repeat sampler would have done, and it also keeps the top of the glass at the top of the picture.
//
// **A door mirror is a curved glass, and it is drawn as one** (docs/driver-mirrors-brief.md §6).
// The centre mirror is flat and its UVs are the whole of where it looks; a door mirror is a convex
// sphere, which the material states as a radius, and its UVs say only where its *centre* looks —
// the island's centre, the axis the modder aimed it along. Everything else is the reflection law:
// the one flat normal that sends the eye's ray at the glass's centre along that axis, the sphere
// tangent to it there, the eye's ray to this fragment reflected off that sphere, and the reflected
// ray projected into the mirror camera's picture through the basis the frame block carries. So the
// middle of the glass shows what the flat glass showed and the edges reach further out, by exactly
// what a sphere of that radius at that distance from the eye reaches, and a straight line seen
// through it bends the way it does in a real door mirror.
//
// The material this draws asks for it by name — the game rebinds the material whose base map is
// `mirror_placement` to this shader when a mirror camera exists (CircuitScene) — so from the chase
// camera, where there is no mirror pass, the surfaces still draw as the asset shipped them.

layout(location = 0) out vec4 fragColor;

// Only what is read, for the reason ColourFragmentShader states. The locations are
// PassThroughVertexShader's own.
layout(location = 0) in vec2 textureCoordinates;
layout(location = 1) in vec3 positionInWorldSpace;

struct Light {
    vec4 position;
    vec4 diffuse;
    vec4 specular;
    vec4 ambientAttenuation;
};

struct Probe {
    vec4 irradiance[SH_COEFFICIENTS];
    vec4 boxMin;
    vec4 boxMax;
    vec4 position;
};

// The whole frame block, declared to the end because the fields this reads are the last ones: a
// uniform block may be a prefix of the buffer backing it, and a prefix must carry everything above
// the field it wants. Between cameraPosition and mirrorParams nothing is read.
layout(set = SET_FRAME, binding = 0) uniform FrameData {
    mat4 viewMatrix;
    vec4 cameraPosition;
    ivec4 lightCount;
    Light lights[MAX_LIGHTS];
    mat4 shadowMatrices[SHADOW_CASCADES];
    vec4 shadowSplits;
    vec4 shadowTexelWorldSize;
    vec4 shadowDepthScale;
    ivec4 shadowParams;
    ivec4 probeParams;
    Probe probes[MAX_IBL_PROBES];
    vec4 fogDensity;
    vec4 fogScatter;
    vec4 fogAmbient;
    vec4 timeRain;
    vec4 rainWind;
    vec4 wiperArcA;
    vec4 wiperArcB;
    vec4 wiperSweep;
    vec4 wiperTiming;
    vec4 wiperPane;
    vec4 rainBody;
    vec4 cloudParams;
    // x what the picture's radiance is multiplied by — the world's exposure over the frame's under
    // the cockpit's split meters, so a mirror in a cabin exposed for the cabin shows the world at
    // the world's number, exactly as the composite lays the world under the windscreen; y one when
    // a real mirror map is bound and zero when the view has none to sample — a scene with no mirror
    // camera, a probe face, or the mirror view itself.
    vec4 mirrorParams;
    // The mirror camera's own view, for the curved glass: xyz where it looks, w the tangent of its
    // half field of view across; then xyz its up, w the tangent of the half field of view down. A
    // scene that aimed no mirror leaves the direction at zero, and the glass then falls back to its
    // UVs rather than divide by it.
    vec4 mirrorAxis;
    vec4 mirrorUp;
} frame;

// The material block, whole, for the two fields at its end: mirrorGlass is xyz the glass's centre in
// the mesh's own space and w the sphere's radius in world units — zero on a flat glass, which is the
// centre mirror and every material that is not a curved one — and mirrorAxis.xy is the centre of the
// glass's UV island, the direction its flat centre reflects the eye along.
layout(set = SET_MATERIAL, binding = 0) uniform MaterialData {
    vec4 baseColour;
    vec4 roughMetal;
    ivec4 useTextures;
    ivec4 useTextures2;
    mat4 textureTransform;
    vec4 blinnPhong;
    vec4 detailTiling;
    vec4 blend;
    vec4 mirrorGlass;
    vec4 mirrorAxis;
} material;

// The draw block, whole, for localToWorld alone: the glass's centre is stated in the mesh's own
// space so that it rides with the car, and this is the matrix that carries it to where the fragment
// is. The block is the vertex stage's, declared whole for the reason PbrFragmentShader gives.
layout(set = SET_DRAW, binding = 0) uniform DrawData {
    mat4 localToWorld;
    mat4 localToView;
    mat4 localToScreen;
    mat4 normalMatrix;
    ivec4 animated;
    vec4 signal;
} draw;

// The rear view, rendered by the mirror camera earlier in this frame. Bound for every shading view
// beside the cascades; only this shader declares it.
layout(set = SET_SHADOW, binding = MIRROR_MAP_BINDING) uniform sampler2D mirrorMap;

// What a first-surface glass mirror gives back. A silvered mirror reflects about nine tenths of
// what falls on it; the rest is the glass and the coating. Placed, and a number the seat may move —
// a picture that reads dim against the cabin is this dial, and one that reads brighter than the
// world seen through the windscreen is the exposure ratio above.
const float mirrorReflectance = 0.9;

// The glass with nothing to show: a dark, unlit pane. Reached only where the map is the dummy —
// the mirror view catching sight of a mirror surface, which the camera's own placement makes
// unlikely — and deliberately not white, which is what an unbound sampler would hand back.
const vec3 mirrorUnlit = vec3(0.02);

// Where a ray from the eye, reflected off the curved glass at this fragment, lands on the rear view;
// x below zero where it does not land in the picture at all (a ray that leaves the glass forward,
// which no glass a driver can see into produces).
vec2 curvedGlassSample(float radius)
{
    vec3 eye = frame.cameraPosition.xyz;

    // The mirror camera's basis: where it looks, its up made orthogonal to that, and its left —
    // the car's left, which the unreversed picture puts on its right, so it is the +u direction.
    vec3 back = normalize(frame.mirrorAxis.xyz);
    vec3 up = normalize(frame.mirrorUp.xyz - back * dot(frame.mirrorUp.xyz, back));
    vec3 left = cross(back, up);
    float tanHalfWidth = frame.mirrorAxis.w;
    float tanHalfHeight = frame.mirrorUp.w;

    // The axis the modder aimed this glass along: its island's centre, as the direction that
    // picture point looks in. v runs down the map, so a centre above the middle is +up.
    vec2 island = material.mirrorAxis.xy;
    vec3 axis = normalize(back + left * ((2.0 * island.x - 1.0) * tanHalfWidth) +
                          up * ((1.0 - 2.0 * island.y) * tanHalfHeight));

    // The one flat normal that sends the eye's ray at the glass's centre along that axis:
    // reflect(d, n) = a has n along a - d, and that n faces the eye whenever a does not point at it.
    vec3 glassCentre = (draw.localToWorld * vec4(material.mirrorGlass.xyz, 1.0)).xyz;
    vec3 toCentre = normalize(glassCentre - eye);
    vec3 flatNormal = normalize(axis - toCentre);

    // The sphere tangent to that plane at the centre, its own centre a radius behind the glass. Its
    // normal at this fragment is the flat one tipped outward by the in-plane offset over the
    // radius — the exact normal of that sphere, not a small-angle stand-in — which is what makes
    // the glass convex towards the eye.
    vec3 offset = positionInWorldSpace - glassCentre;
    offset -= flatNormal * dot(offset, flatNormal);
    vec3 normal = normalize(flatNormal + offset / radius);
    vec3 seen = reflect(normalize(positionInWorldSpace - eye), normal);

    // Into the picture: the perspective projection the mirror camera drew with, written out in its
    // own basis. The map's row 0 is the top of the scene, so up is -v.
    float depth = dot(seen, back);

    if (depth <= 1e-4)
    {
        return vec2(-1.0);
    }

    return vec2(0.5 + 0.5 * dot(seen, left) / (depth * tanHalfWidth),
                0.5 - 0.5 * dot(seen, up) / (depth * tanHalfHeight));
}

void main()
{
    if (frame.mirrorParams.y <= 0.0)
    {
        fragColor = vec4(mirrorUnlit, 1.0);

        return;
    }

    // A flat glass looks where its UVs say. A curved one looks where its curve says, given a mirror
    // view to project into; without one — a scene that bound a map and aimed nothing — it is flat.
    vec2 sampleAt = fract(textureCoordinates);
    bool curved = material.mirrorGlass.w > 0.0 && dot(frame.mirrorAxis.xyz, frame.mirrorAxis.xyz) > 1e-8;

    if (curved)
    {
        sampleAt = curvedGlassSample(material.mirrorGlass.w);

        if (sampleAt.x < 0.0)
        {
            fragColor = vec4(mirrorUnlit, 1.0);

            return;
        }
    }

    vec3 seen = texture(mirrorMap, sampleAt).rgb;

    fragColor = vec4(seen * (mirrorReflectance * frame.mirrorParams.x), 1.0);
}
