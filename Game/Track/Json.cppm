module;

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module osr.game:Json;

// A strict JSON reader, written here rather than taken from a library for the reason `SetupFile`
// parses its own sheet by hand: the sandbox names no third-party headers, and a header-only parser
// in a global module fragment is paid for by every unit that imports this one.
//
// **It lived inside `TrafficNetwork.cppm` until 2026-09-06** and moved out unchanged the day the
// collider manifest became a second document that needed it. Nothing here knows what it is reading.
//
// This unit imports nothing, which is the `Options.cppm` and `CloudNoise.cppm` pattern and is what
// keeps `<charconv>` out of a global module fragment that would otherwise be merged against every
// imported BMI. Full account: docs/build-times.md.

namespace osr
{

// A JSON document as a tree, and nothing wider than this file needs.
//
// Objects keep their members as two parallel vectors rather than as a `std::map`, and that is not a
// style choice: `<map>` in a second global module fragment moves the owning module of libstdc++'s
// `_Rb_tree_iterator` hidden friends, and the sandbox then fails to link with an undefined
// `operator==` out of a translation unit that never mentioned a map (CLAUDE.md, *Do not break*).
export enum class JsonKind : std::uint8_t {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object
};

export struct JsonValue
{
    JsonKind kind = JsonKind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    // An array's elements, or an object's values in the order the document states them.
    std::vector<JsonValue> items;
    // An object's keys, one per `items` entry. Empty for everything else.
    std::vector<std::string> keys;
};

// Made rather than brace-initialised, because a designated initialiser here would have to name
// every field this reader does not care about: `-Wmissing-designated-field-initializers` is an
// error and `text`, `items` and `keys` carry no default member initialiser of their own.
export [[nodiscard]] JsonValue jsonOf(const JsonKind kind)
{
    auto made = JsonValue{};
    made.kind = kind;

    return made;
}

export [[nodiscard]] JsonValue jsonBoolean(const bool value)
{
    auto made = jsonOf(JsonKind::Boolean);
    made.boolean = value;

    return made;
}

export [[nodiscard]] JsonValue jsonNumber(const double value)
{
    auto made = jsonOf(JsonKind::Number);
    made.number = value;

    return made;
}

export [[nodiscard]] JsonValue jsonString(std::string value)
{
    auto made = jsonOf(JsonKind::String);
    made.text = std::move(value);

    return made;
}

export [[nodiscard]] const JsonValue* member(const JsonValue& value, const std::string_view key)
{
    if (value.kind != JsonKind::Object)
    {
        return nullptr;
    }

    for (auto index = std::size_t{0}; index < value.keys.size(); index++)
    {
        if (value.keys[index] == key)
        {
            return &value.items[index];
        }
    }

    return nullptr;
}

export [[nodiscard]] double numberFrom(const JsonValue& parent, const std::string_view key, const double fallback)
{
    const auto* found = member(parent, key);

    return (found != nullptr && found->kind == JsonKind::Number) ? found->number : fallback;
}

export [[nodiscard]] int integerFrom(const JsonValue& parent, const std::string_view key, const int fallback)
{
    const auto* found = member(parent, key);

    return (found != nullptr && found->kind == JsonKind::Number) ? static_cast<int>(found->number) : fallback;
}

export [[nodiscard]] bool booleanFrom(const JsonValue& parent, const std::string_view key, const bool fallback)
{
    const auto* found = member(parent, key);

    return (found != nullptr && found->kind == JsonKind::Boolean) ? found->boolean : fallback;
}

export [[nodiscard]] std::string textFrom(const JsonValue& parent, const std::string_view key)
{
    const auto* found = member(parent, key);

    return (found != nullptr && found->kind == JsonKind::String) ? found->text : std::string();
}

export void appendUtf8(std::string& target, const std::uint32_t point)
{
    if (point < 0x80u)
    {
        target.push_back(static_cast<char>(point));
    }
    else if (point < 0x800u)
    {
        target.push_back(static_cast<char>(0xC0u | (point >> 6u)));
        target.push_back(static_cast<char>(0x80u | (point & 0x3Fu)));
    }
    else if (point < 0x10000u)
    {
        target.push_back(static_cast<char>(0xE0u | (point >> 12u)));
        target.push_back(static_cast<char>(0x80u | ((point >> 6u) & 0x3Fu)));
        target.push_back(static_cast<char>(0x80u | (point & 0x3Fu)));
    }
    else
    {
        target.push_back(static_cast<char>(0xF0u | (point >> 18u)));
        target.push_back(static_cast<char>(0x80u | ((point >> 12u) & 0x3Fu)));
        target.push_back(static_cast<char>(0x80u | ((point >> 6u) & 0x3Fu)));
        target.push_back(static_cast<char>(0x80u | (point & 0x3Fu)));
    }
}

// A strict recursive-descent reader. Strict because the alternative is a loader that accepts a
// truncated export and hands back a shorter city.
export class JsonReader
{
public:
    explicit JsonReader(const std::string& document) : text(document)
    {
    }

    [[nodiscard]] std::expected<JsonValue, std::string> read()
    {
        auto value = readValue(0);
        if (!value)
        {
            return value;
        }

        skipBlanks();
        if (at != text.size())
        {
            return std::unexpected(problem("trailing characters after the document"));
        }

        return value;
    }

private:
    // Deep enough for anything a tool writes and shallow enough that a malformed document cannot
    // run the stack out: this reader recurses once per level of nesting.
    static constexpr unsigned int maximumDepth = 64;

    std::string_view text;
    std::size_t at = 0;

    [[nodiscard]] std::string problem(const std::string_view what) const
    {
        return std::string(what).append(", at byte ").append(std::to_string(at));
    }

    void skipBlanks()
    {
        while (at < text.size() &&
               (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
        {
            at++;
        }
    }

    [[nodiscard]] bool ahead(const std::string_view word) const
    {
        return text.substr(at).starts_with(word);
    }

    [[nodiscard]] std::expected<JsonValue, std::string> readValue(const unsigned int depth)
    {
        if (depth > maximumDepth)
        {
            return std::unexpected(problem("nesting is deeper than this reader accepts"));
        }

        skipBlanks();
        if (at >= text.size())
        {
            return std::unexpected(problem("a value was expected and the document ended"));
        }

        if (text[at] == '{')
        {
            return readObject(depth);
        }

        if (text[at] == '[')
        {
            return readArray(depth);
        }

        if (text[at] == '"')
        {
            auto value = readString();
            if (!value)
            {
                return std::unexpected(std::move(value).error());
            }

            return jsonString(std::move(value).value());
        }

        if (ahead("true"))
        {
            at += 4;
            return jsonBoolean(true);
        }

        if (ahead("false"))
        {
            at += 5;
            return jsonBoolean(false);
        }

        if (ahead("null"))
        {
            at += 4;
            return JsonValue{};
        }

        return readNumber();
    }

    [[nodiscard]] std::expected<JsonValue, std::string> readNumber()
    {
        const auto begin = at;

        while (at < text.size())
        {
            const auto character = text[at];
            const auto part = (character >= '0' && character <= '9') || character == '-' || character == '+' ||
                              character == '.' || character == 'e' || character == 'E';
            if (!part)
            {
                break;
            }

            at++;
        }

        if (begin == at)
        {
            return std::unexpected(problem("a value was expected"));
        }

        auto parsed = 0.0;
        const auto* first = text.data() + begin;
        const auto* last = text.data() + at;
        const auto answer = std::from_chars(first, last, parsed);

        if (answer.ec != std::errc{} || answer.ptr != last)
        {
            return std::unexpected(problem("a number this reader cannot read"));
        }

        return jsonNumber(parsed);
    }

    [[nodiscard]] std::expected<std::uint32_t, std::string> readHexQuad()
    {
        if (at + 4 > text.size())
        {
            return std::unexpected(problem("a \\u escape is short"));
        }

        auto point = std::uint32_t{0};

        for (auto index = std::size_t{0}; index < 4; index++)
        {
            const auto character = text[at + index];
            auto digit = std::uint32_t{0};

            if (character >= '0' && character <= '9')
            {
                digit = static_cast<std::uint32_t>(character - '0');
            }
            else if (character >= 'a' && character <= 'f')
            {
                digit = static_cast<std::uint32_t>(character - 'a') + 10u;
            }
            else if (character >= 'A' && character <= 'F')
            {
                digit = static_cast<std::uint32_t>(character - 'A') + 10u;
            }
            else
            {
                return std::unexpected(problem("a \\u escape is not four hex digits"));
            }

            point = (point << 4u) | digit;
        }

        at += 4;

        return point;
    }

    [[nodiscard]] std::expected<std::uint32_t, std::string> readCodePoint()
    {
        auto leading = readHexQuad();
        if (!leading)
        {
            return leading;
        }

        auto point = leading.value();

        // A surrogate pair, and only where the second half really is one — an unpaired high
        // surrogate is left as it stands rather than swallowing the escape that followed it.
        if (point >= 0xD800u && point <= 0xDBFFu && ahead("\\u"))
        {
            const auto mark = at;
            at += 2;

            auto trailing = readHexQuad();
            if (!trailing)
            {
                return trailing;
            }

            if (trailing.value() >= 0xDC00u && trailing.value() <= 0xDFFFu)
            {
                point = 0x10000u + ((point - 0xD800u) << 10u) + (trailing.value() - 0xDC00u);
            }
            else
            {
                at = mark;
            }
        }

        return point;
    }

    [[nodiscard]] std::expected<std::string, std::string> readString()
    {
        // The opening quote, which the caller has already seen.
        at++;

        auto value = std::string();

        while (true)
        {
            if (at >= text.size())
            {
                return std::unexpected(problem("a string was not closed"));
            }

            const auto character = text[at];

            if (character == '"')
            {
                at++;
                return value;
            }

            if (character != '\\')
            {
                value.push_back(character);
                at++;
                continue;
            }

            at++;
            if (at >= text.size())
            {
                return std::unexpected(problem("an escape was not completed"));
            }

            const auto escape = text[at];
            at++;

            switch (escape)
            {
            case '"':
                value.push_back('"');
                break;
            case '\\':
                value.push_back('\\');
                break;
            case '/':
                value.push_back('/');
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u': {
                auto point = readCodePoint();
                if (!point)
                {
                    return std::unexpected(std::move(point).error());
                }

                appendUtf8(value, point.value());
                break;
            }
            default:
                return std::unexpected(problem("an escape this reader does not know"));
            }
        }
    }

    [[nodiscard]] std::expected<JsonValue, std::string> readArray(const unsigned int depth)
    {
        // The opening bracket.
        at++;

        auto value = jsonOf(JsonKind::Array);

        skipBlanks();
        if (at < text.size() && text[at] == ']')
        {
            at++;
            return value;
        }

        while (true)
        {
            auto element = readValue(depth + 1);
            if (!element)
            {
                return std::unexpected(std::move(element).error());
            }

            value.items.push_back(std::move(element).value());

            skipBlanks();
            if (at >= text.size())
            {
                return std::unexpected(problem("an array was not closed"));
            }

            if (text[at] == ',')
            {
                at++;
                continue;
            }

            if (text[at] == ']')
            {
                at++;
                return value;
            }

            return std::unexpected(problem("an array element is not followed by ',' or ']'"));
        }
    }

    [[nodiscard]] std::expected<JsonValue, std::string> readObject(const unsigned int depth)
    {
        // The opening brace.
        at++;

        auto value = jsonOf(JsonKind::Object);

        skipBlanks();
        if (at < text.size() && text[at] == '}')
        {
            at++;
            return value;
        }

        while (true)
        {
            skipBlanks();
            if (at >= text.size() || text[at] != '"')
            {
                return std::unexpected(problem("an object member does not start with a key"));
            }

            auto key = readString();
            if (!key)
            {
                return std::unexpected(std::move(key).error());
            }

            skipBlanks();
            if (at >= text.size() || text[at] != ':')
            {
                return std::unexpected(problem("an object key is not followed by ':'"));
            }

            at++;

            auto element = readValue(depth + 1);
            if (!element)
            {
                return std::unexpected(std::move(element).error());
            }

            value.keys.push_back(std::move(key).value());
            value.items.push_back(std::move(element).value());

            skipBlanks();
            if (at >= text.size())
            {
                return std::unexpected(problem("an object was not closed"));
            }

            if (text[at] == ',')
            {
                at++;
                continue;
            }

            if (text[at] == '}')
            {
                at++;
                return value;
            }

            return std::unexpected(problem("an object member is not followed by ',' or '}'"));
        }
    }
};

} // namespace osr
