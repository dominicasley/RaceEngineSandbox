module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

export module osr.game:Font;

import :Json;

import raceengine;

namespace osr
{

// One glyph of a baked font: where it sits against the pen in ems (y down, so the top of a capital
// is negative), how far the pen moves, and where its distance field is in the atlas, 0..1.
export struct Glyph
{
    std::uint32_t codePoint = 0;
    float advance = 0.0f;
    glm::vec2 planeMin{0.0f};
    glm::vec2 planeMax{0.0f};
    glm::vec2 uvMin{0.0f};
    glm::vec2 uvMax{0.0f};
    // A glyph with no outline — the space — advances and draws nothing.
    bool blank = true;
};

export enum class TextAlign
{
    Left,
    Centre,
    Right
};

// A font the canvas draws text with: the multi-channel signed-distance-field atlas
// scripts/bake-font.py wrote and its glyph table, read once, laid out into `CanvasQuad`s on demand.
// The dashboard's readouts are its first reader and a UI is its second; nothing here knows which
// (docs/instrument-cluster-brief.md §6).
//
// A run is laid out one glyph after another along a baseline: the pen advances by each glyph's
// advance (no kerning — the bake carries none for this font, and its figures are tabular to a
// thousandth of an em, which is what a readout needs so digits do not walk as they change), and
// each glyph's quad is its plane bounds scaled to the size and placed at the pen. Sizes are pixels
// per em on the canvas the quads are drawn on; the field's antialiasing takes care of the rest.
export class Font
{
public:
    // `stem` names `<stem>.json` and `<stem>.png` — the bake's two files.
    Font(raceengine::Engine& engine, const std::string& stem);

    [[nodiscard]] raceengine::Resource<raceengine::Texture> atlas() const
    {
        return atlasTexture;
    }

    // The atlas's distance range in its own texels — what the canvas batch states so the fragment
    // stage can resolve the edge at any size.
    [[nodiscard]] float distanceRange() const
    {
        return range;
    }

    [[nodiscard]] float lineHeight() const
    {
        return line;
    }

    // Ems from the baseline to the top of the tallest ascender (positive, up) and to the bottom
    // of the deepest descender (positive, down).
    [[nodiscard]] float ascender() const
    {
        return ascent;
    }

    [[nodiscard]] float descender() const
    {
        return descent;
    }

    // An empty text batch bound to this atlas, for the quads `layout` appends.
    [[nodiscard]] raceengine::CanvasBatch batch() const
    {
        return raceengine::CanvasBatch{.kind = raceengine::CanvasBatchKind::Text,
                                       .image = atlasTexture,
                                       .distanceRange = range,
                                       .quads = {}};
    }

    // A run's width in ems (UTF-8; a code point the atlas lacks advances as a space).
    [[nodiscard]] float measure(std::string_view text) const;

    // Lays `text` on the baseline through `origin` (canvas pixels), `size` pixels per em, aligned
    // about the origin, tinted `colour` (straight alpha), appending one quad per visible glyph.
    void layout(std::string_view text, glm::vec2 origin, float size, TextAlign align, const glm::vec4& colour,
                std::vector<raceengine::CanvasQuad>& out) const;

private:
    raceengine::Resource<raceengine::Texture> atlasTexture;
    float range = 0.0f;
    float line = 1.0f;
    float ascent = 0.8f;
    float descent = 0.2f;
    // Sorted by code point; found by binary search, which is what keeps `<map>` out of this unit
    // (CLAUDE.md, the second-GMF `<map>` link failure).
    std::vector<Glyph> glyphs;
    Glyph fallback{};

    [[nodiscard]] const Glyph& glyph(std::uint32_t codePoint) const;

    // The next code point of a UTF-8 string from `at`, advancing it; a malformed byte reads as '?'.
    [[nodiscard]] static std::uint32_t decode(std::string_view text, std::size_t& at);
};

} // namespace osr

namespace osr
{

Font::Font(raceengine::Engine& engine, const std::string& stem)
{
    const auto document = engine.resource().loadTextFile(stem + ".json");
    if (!document)
    {
        raceengine::fail("the font's glyph table " + stem + ".json did not load: " + document.error());
    }
    auto reader = JsonReader(document.value());
    const auto root = reader.read();
    if (!root)
    {
        raceengine::fail("the font's glyph table " + stem + ".json did not parse: " + root.error());
    }

    const auto* atlas = member(root.value(), "atlas");
    const auto* metrics = member(root.value(), "metrics");
    const auto* table = member(root.value(), "glyphs");
    if (atlas == nullptr || metrics == nullptr || table == nullptr || table->kind != JsonKind::Array)
    {
        raceengine::fail("the font's glyph table " + stem + ".json is not msdf-atlas-gen's layout");
    }
    if (textFrom(*atlas, "yOrigin") != "top")
    {
        raceengine::fail("the font " + stem + " was baked with the atlas's y origin at the bottom; bake with -yorigin top");
    }

    const auto width = numberFrom(*atlas, "width", 1.0);
    const auto height = numberFrom(*atlas, "height", 1.0);
    range = static_cast<float>(numberFrom(*atlas, "distanceRange", 0.0));
    line = static_cast<float>(numberFrom(*metrics, "lineHeight", 1.0));
    // With the y origin at the top the bake states the ascender negative (up) and the descender
    // positive (down); both are carried here as the magnitudes they are.
    ascent = static_cast<float>(-numberFrom(*metrics, "ascender", -0.8));
    descent = static_cast<float>(numberFrom(*metrics, "descender", 0.2));

    glyphs.reserve(table->items.size());
    for (const auto& entry : table->items)
    {
        Glyph glyph{};
        glyph.codePoint = static_cast<std::uint32_t>(integerFrom(entry, "unicode", 0));
        glyph.advance = static_cast<float>(numberFrom(entry, "advance", 0.0));
        const auto* plane = member(entry, "planeBounds");
        const auto* bounds = member(entry, "atlasBounds");
        if (plane != nullptr && bounds != nullptr)
        {
            glyph.planeMin = glm::vec2(numberFrom(*plane, "left", 0.0), numberFrom(*plane, "top", 0.0));
            glyph.planeMax = glm::vec2(numberFrom(*plane, "right", 0.0), numberFrom(*plane, "bottom", 0.0));
            glyph.uvMin = glm::vec2(numberFrom(*bounds, "left", 0.0) / width, numberFrom(*bounds, "top", 0.0) / height);
            glyph.uvMax =
                glm::vec2(numberFrom(*bounds, "right", 0.0) / width, numberFrom(*bounds, "bottom", 0.0) / height);
            glyph.blank = false;
        }
        glyphs.push_back(glyph);
    }
    std::sort(glyphs.begin(), glyphs.end(), [](const Glyph& a, const Glyph& b) { return a.codePoint < b.codePoint; });
    fallback = glyph(static_cast<std::uint32_t>(' '));

    const auto texture = engine.resource().loadTexture(stem + ".png");
    if (!texture)
    {
        raceengine::fail("the font's atlas " + stem + ".png did not load: " + texture.error());
    }
    atlasTexture = texture.value();
    // A distance field is data, not colour: sampled linear whatever the file says.
    engine.memoryStorage().textures.mutate(
        atlasTexture, [](raceengine::Texture& image) { image.colourSpace = raceengine::ColourSpace::Linear; });

    engine.log().info("Font {}: {} glyphs, atlas {}x{} at {} texels per em, range {}", stem, glyphs.size(), width,
                      height, numberFrom(*atlas, "size", 0.0), range);
}

const Glyph& Font::glyph(const std::uint32_t codePoint) const
{
    const auto found = std::lower_bound(glyphs.begin(), glyphs.end(), codePoint,
                                        [](const Glyph& entry, const std::uint32_t point) { return entry.codePoint < point; });
    if (found == glyphs.end() || found->codePoint != codePoint)
    {
        return fallback;
    }
    return *found;
}

std::uint32_t Font::decode(const std::string_view text, std::size_t& at)
{
    const auto lead = static_cast<unsigned char>(text[at]);
    auto length = std::size_t{1};
    auto point = std::uint32_t{lead};
    if (lead >= 0xF0)
    {
        length = 4;
        point = lead & 0x07u;
    }
    else if (lead >= 0xE0)
    {
        length = 3;
        point = lead & 0x0Fu;
    }
    else if (lead >= 0xC0)
    {
        length = 2;
        point = lead & 0x1Fu;
    }
    if (at + length > text.size())
    {
        at = text.size();
        return static_cast<std::uint32_t>('?');
    }
    for (auto index = std::size_t{1}; index < length; index++)
    {
        point = (point << 6u) | (static_cast<unsigned char>(text[at + index]) & 0x3Fu);
    }
    at += length;
    return point;
}

float Font::measure(const std::string_view text) const
{
    auto width = 0.0f;
    auto at = std::size_t{0};
    while (at < text.size())
    {
        width += glyph(decode(text, at)).advance;
    }
    return width;
}

void Font::layout(const std::string_view text, const glm::vec2 origin, const float size, const TextAlign align,
                  const glm::vec4& colour, std::vector<raceengine::CanvasQuad>& out) const
{
    auto pen = origin.x;
    if (align != TextAlign::Left)
    {
        const auto width = measure(text) * size;
        pen -= align == TextAlign::Centre ? 0.5f * width : width;
    }
    auto at = std::size_t{0};
    while (at < text.size())
    {
        const auto& entry = glyph(decode(text, at));
        if (!entry.blank)
        {
            out.push_back(raceengine::CanvasQuad{
                .position = glm::vec2(pen, origin.y) + entry.planeMin * size,
                .size = (entry.planeMax - entry.planeMin) * size,
                .uvMin = entry.uvMin,
                .uvMax = entry.uvMax,
                .colour = colour});
        }
        pen += entry.advance * size;
    }
}

} // namespace osr
