#version 450

// The canvas pass's fragment stage: one image per batch, and two ways to read it.
//
// A **text** batch's image is a multi-channel signed distance field (msdf-atlas-gen's, three
// channels each a signed distance to one of the glyph's edge segments, 0.5 on the edge); the
// median of the three is the true distance, which keeps corners sharp where a single-channel field
// rounds them. The edge is resolved at the pixel: the field's range in image units times the
// image's size on screen (the reciprocal of the UV's derivative) is how many pixels one unit of
// distance spans, and one pixel of that around 0.5 is the antialiased edge — so one atlas serves
// every size the quads are drawn at, and a glyph the size of the screen has the same edge as one
// ten pixels tall. (Chlumský, "Shape Decomposition for Multi-channel Distance Fields", 2015.)
//
// An **image** batch samples the picture as it is; with no image bound the white dummy makes the
// quad its tint alone, which is a solid rectangle.
//
// Both hand back **premultiplied** colour — the pipeline blends ONE / ONE_MINUS_SRC_ALPHA — and when
// the target is the backbuffer, whose UNORM format shows its bytes straight, the display's own
// transfer curve is applied here, the way the presenter does for the picture under it.

layout(set = SET_CANVAS_IMAGE, binding = CANVAS_IMAGE_BINDING) uniform sampler2D image;

layout(push_constant) uniform Canvas
{
    vec2 canvasSize;
    vec2 unitRange;
    uint firstQuad;
    uint kind;
    uint encode;
    uint reserved;
} canvas;

layout(location = 0) in vec2 imageCoordinates;
layout(location = 1) in vec4 tint;

layout(location = 0) out vec4 fragColor;

float median(float r, float g, float b)
{
    return max(min(r, g), min(max(r, g), b));
}

vec3 encodeSrgb(vec3 linear)
{
    vec3 low = linear * 12.92;
    vec3 high = 1.055 * pow(max(linear, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(low, high, step(vec3(0.0031308), linear));
}

void main()
{
    vec4 colour;
    if (canvas.kind == 1u)
    {
        vec3 distances = texture(image, imageCoordinates).rgb;
        float signedDistance = median(distances.r, distances.g, distances.b);
        vec2 screenTexelSize = vec2(1.0) / max(fwidth(imageCoordinates), vec2(1e-6));
        float screenPixelRange = max(0.5 * dot(canvas.unitRange, screenTexelSize), 1.0);
        float screenPixelDistance = screenPixelRange * (signedDistance - 0.5);
        float coverage = clamp(screenPixelDistance + 0.5, 0.0, 1.0);
        colour = vec4(tint.rgb, 1.0) * (tint.a * coverage);
    }
    else
    {
        vec4 sampled = texture(image, imageCoordinates) * tint;
        colour = vec4(sampled.rgb * sampled.a, sampled.a);
    }
    if (canvas.encode == 1u)
    {
        colour.rgb = encodeSrgb(colour.rgb);
    }
    fragColor = colour;
}
