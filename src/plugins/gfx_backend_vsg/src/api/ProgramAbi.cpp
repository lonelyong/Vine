#include <vine/vsg/api/ProgramAbi.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

VN_VSG_NS_BEGIN

namespace
{

/// @brief Whether @p character may appear in a GLSL identifier (the scan's word characters).
bool isNameChar(char character) noexcept
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_';
}

/** @brief Gets @p text without leading and trailing spaces (newlines included: a declaration may wrap). */
std::string_view trim(std::string_view text) noexcept
{
    std::size_t begin = 0;
    std::size_t end   = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n'))
    {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

/** @brief Moves @p at past spaces and tabs; returns the new position. */
std::size_t skipSpaces(std::string_view text, std::size_t at) noexcept
{
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
    {
        ++at;
    }
    return at;
}

/**
 * @brief Reads the identifier at @p at (skipping spaces first).
 *
 * @param text Text to read from.
 * @param at   Position to read at; moved past the word when one was read.
 * @param word Receives the word.
 * @return true when a word was read; false leaves @p at on the first non-space character.
 */
bool readWord(std::string_view text, std::size_t& at, std::string_view& word) noexcept
{
    const std::size_t begin = skipSpaces(text, at);
    std::size_t       end   = begin;
    while (end < text.size() && isNameChar(text[end]))
    {
        ++end;
    }
    if (end == begin || (text[begin] >= '0' && text[begin] <= '9'))
    {
        at = begin;
        return false;
    }
    word = text.substr(begin, end - begin);
    at   = end;
    return true;
}

/** @brief Parses @p text as an unsigned decimal literal (the whole text). */
bool parseUnsigned(std::string_view text, std::uint32_t& out) noexcept
{
    const std::string_view digits = trim(text);
    if (digits.empty())
    {
        return false;
    }
    std::uint64_t value = 0;
    for (const char character : digits)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }
        value = value * 10U + static_cast<std::uint64_t>(character - '0');
        if (value > 0xFFFFFFFFULL)
        {
            return false;
        }
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

/** @brief Replaces every comment with spaces (newlines kept, so the line structure survives the scan). */
std::string stripComments(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t at = 0; at < text.size();)
    {
        const bool line_comment   = text[at] == '/' && at + 1U < text.size() && text[at + 1U] == '/';
        const bool block_comment  = text[at] == '/' && at + 1U < text.size() && text[at + 1U] == '*';
        if (line_comment)
        {
            while (at < text.size() && text[at] != '\n')
            {
                out.push_back(' ');
                ++at;
            }
        }
        else if (block_comment)
        {
            out.append("  ");
            at += 2U;
            while (at < text.size() && !(text[at] == '*' && at + 1U < text.size() && text[at + 1U] == '/'))
            {
                out.push_back(text[at] == '\n' ? '\n' : ' ');
                ++at;
            }
            if (at < text.size())
            {
                out.append("  ");
                at += 2U;
            }
        }
        else
        {
            out.push_back(text[at]);
            ++at;
        }
    }
    return out;
}

/** @brief Evaluates a `defined(X)` / `defined X` condition (optionally negated with a leading `!`). */
bool evaluateDefined(std::string_view text, const std::unordered_set<std::string>& defines, bool& out)
{
    std::string_view rest = trim(text);
    bool             negate = false;
    if (!rest.empty() && rest.front() == '!')
    {
        negate = true;
        rest   = trim(rest.substr(1U));
    }
    if (rest.substr(0U, 7U) != "defined")
    {
        return false;   // an expression this scan does not read is reported, never guessed
    }
    rest = trim(rest.substr(7U));

    std::string_view name;
    if (!rest.empty() && rest.front() == '(')
    {
        const std::size_t close = rest.find(')');
        if (close == std::string_view::npos)
        {
            return false;
        }
        name = trim(rest.substr(1U, close - 1U));
        rest = trim(rest.substr(close + 1U));
    }
    else
    {
        std::size_t at = 0;
        if (!readWord(rest, at, name))
        {
            return false;
        }
        rest = trim(rest.substr(at));
    }
    if (name.empty() || !rest.empty())
    {
        return false;   // `defined(A) && ...`: a second operator is outside this scan's grammar
    }
    out = defines.contains(std::string(name)) != negate;
    return true;
}

/** @brief One `#if` group's bookkeeping (nesting included). */
struct ConditionalBranch
{
    bool parent_active{true};   ///< Whether the text around the group is taken.
    bool taken{false};          ///< Whether any branch of this group has been taken.
    bool active{true};          ///< Whether THIS branch is the taken one.
    bool else_seen{false};      ///< Whether `#else` was already read (a later `#elif` is then an error).
};

/** @brief What the conditional pass produced: the taken lines, or why it could not answer. */
struct ConditionalResult
{
    enum class Outcome
    {
        Ok,              ///< Every conditional was classified.
        Unclassifiable,  ///< A conditional expression outside the scan's grammar was found.
        Refused,         ///< A `#error` in a taken branch: the text says this variant does not compile.
    };

    Outcome                  outcome{Outcome::Ok};
    std::string              active;          ///< The lines of the taken branches, directives removed.
    std::vector<std::string> import_defines;  ///< The names the taken `#pragma import_defines` allowed.
};

/** @brief Keeps the lines of @p text whose conditional branches are taken under @p defines. */
ConditionalResult applyConditionals(std::string_view text, const std::unordered_set<std::string>& defines)
{
    ConditionalResult               result;
    std::unordered_set<std::string> active_defines = defines;
    std::vector<ConditionalBranch>  stack;
    bool                            active_now = true;

    for (std::size_t at = 0; at <= text.size();)
    {
        const std::size_t newline = text.find('\n', at);
        const std::size_t end     = newline == std::string_view::npos ? text.size() : newline;
        const std::string_view line = text.substr(at, end - at);
        at = newline == std::string_view::npos ? text.size() + 1U : newline + 1U;

        const std::string_view trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() != '#')
        {
            if (active_now)
            {
                result.active.append(line);
                result.active.push_back('\n');
            }
            continue;
        }

        std::size_t      cursor = 1U;
        std::string_view directive;
        if (!readWord(trimmed, cursor, directive))
        {
            continue;   // a lone `#` is not a directive
        }
        const std::string_view rest = trim(trimmed.substr(cursor));

        if (directive == "if" || directive == "ifdef" || directive == "ifndef" || directive == "elif")
        {
            bool value     = false;
            bool classified = false;
            if (directive == "ifdef" || directive == "ifndef")
            {
                std::size_t      probe = 0U;
                std::string_view name;
                classified = readWord(rest, probe, name) && trim(rest.substr(probe)).empty();
                value      = classified && active_defines.contains(std::string(name)) != (directive == "ifndef");
            }
            else
            {
                classified = evaluateDefined(rest, active_defines, value);
            }
            if (!classified || (directive == "elif" && (stack.empty() || stack.back().else_seen)))
            {
                result.outcome = ConditionalResult::Outcome::Unclassifiable;
                return result;
            }

            if (directive == "elif")
            {
                ConditionalBranch& branch = stack.back();
                branch.active             = branch.parent_active && !branch.taken && value;
                branch.taken              = branch.taken || value;
            }
            else
            {
                stack.push_back(ConditionalBranch{ active_now, value, active_now && value, false });
            }
            active_now = stack.back().active;
        }
        else if (directive == "else")
        {
            if (stack.empty() || stack.back().else_seen)
            {
                result.outcome = ConditionalResult::Outcome::Unclassifiable;
                return result;
            }
            ConditionalBranch& branch = stack.back();
            branch.active             = branch.parent_active && !branch.taken;
            branch.taken              = true;
            branch.else_seen          = true;
            active_now                = branch.active;
        }
        else if (directive == "endif")
        {
            if (stack.empty())
            {
                result.outcome = ConditionalResult::Outcome::Unclassifiable;
                return result;
            }
            stack.pop_back();
            active_now = stack.empty() ? true : stack.back().active;
        }
        else if (directive == "error")
        {
            if (active_now)
            {
                result.outcome = ConditionalResult::Outcome::Refused;
                return result;
            }
        }
        else if (directive == "define" || directive == "undef")
        {
            std::size_t      probe = 0U;
            std::string_view name;
            if (active_now && readWord(rest, probe, name))
            {
                if (directive == "define")
                {
                    active_defines.insert(std::string(name));
                }
                else
                {
                    active_defines.erase(std::string(name));
                }
            }
        }
        else if (directive == "pragma" && active_now)
        {
            std::size_t      probe = 0U;
            std::string_view name;
            if (readWord(rest, probe, name) && name == "import_defines")
            {
                std::string_view list = trim(rest.substr(probe));
                if (!list.empty() && list.front() == '(')
                {
                    const std::size_t close = list.find(')');
                    list = close == std::string_view::npos ? std::string_view{} : list.substr(1U, close - 1U);
                }
                for (std::size_t item = 0U; item < list.size();)
                {
                    const std::size_t comma = list.find(',', item);
                    const std::string_view entry = trim(list.substr(item, (comma == std::string_view::npos ? list.size() : comma) - item));
                    if (!entry.empty())
                    {
                        result.import_defines.emplace_back(entry);
                    }
                    item = comma == std::string_view::npos ? list.size() : comma + 1U;
                }
            }
        }
        // Every other directive (#version, #extension, ...) states nothing about bindings.
    }

    if (!stack.empty())
    {
        result.outcome = ConditionalResult::Outcome::Unclassifiable;   // an unterminated conditional
    }
    return result;
}

/** @brief The layout qualifiers one `layout(...)` list carries, for the declarations this scan reads. */
struct Qualifiers
{
    bool          classified{true};   ///< false when a value the scan does not read was found.
    bool          has_set{false};     ///< Whether a `set = N` qualifier was present.
    std::uint32_t set{0};             ///< The set it named.
    bool          has_binding{false}; ///< Whether a `binding = N` qualifier was present.
    std::uint32_t binding{0};         ///< The binding it named.
    bool          has_offset{false};  ///< Whether an `offset = N` qualifier was present.
    std::uint32_t offset{0};          ///< The offset it named.
    bool          push_constant{false};   ///< Whether the list says `push_constant`.
    AbiBlockLayout layout{AbiBlockLayout::Unspecified};   ///< The block layout it named.
};

/** @brief Parses one `layout(...)` qualifier list. */
Qualifiers parseQualifiers(std::string_view text)
{
    Qualifiers result;
    for (std::size_t at = 0U; at < text.size();)
    {
        const std::size_t comma = text.find(',', at);
        const std::string_view item = trim(text.substr(at, (comma == std::string_view::npos ? text.size() : comma) - at));
        at = comma == std::string_view::npos ? text.size() : comma + 1U;
        if (item.empty())
        {
            continue;
        }

        const std::size_t      equals = item.find('=');
        const std::string_view key    = trim(item.substr(0U, equals == std::string_view::npos ? item.size() : equals));
        if (equals == std::string_view::npos)
        {
            if (key == "std140")
            {
                result.layout = AbiBlockLayout::Std140;
            }
            else if (key == "std430")
            {
                result.layout = AbiBlockLayout::Std430;
            }
            else if (key == "push_constant")
            {
                result.push_constant = true;
            }
            // shared / packed / scalar / row_major and the rest state nothing this scan reads.
            continue;
        }

        const std::string_view value = item.substr(equals + 1U);
        if (key == "set" || key == "binding" || key == "offset")
        {
            std::uint32_t number = 0;
            if (!parseUnsigned(value, number))
            {
                result.classified = false;   // a binding this scan cannot read is not a binding to guess at
                return result;
            }
            if (key == "set")
            {
                result.has_set = true;
                result.set     = number;
            }
            else if (key == "binding")
            {
                result.has_binding = true;
                result.binding     = number;
            }
            else
            {
                result.has_offset = true;
                result.offset     = number;
            }
        }
        // location / index / constant_id / local_size_* say nothing about descriptor bindings.
    }
    return result;
}

/** @brief Rounds @p value up to the next multiple of @p alignment. */
std::uint32_t roundUp(std::uint32_t value, std::uint32_t alignment) noexcept
{
    return alignment == 0U ? value : (value + alignment - 1U) / alignment * alignment;
}

/** @brief The std140/std430 size and alignment of one member type, when the scan knows the type. */
bool memberTypeLayout(std::string_view type, std::uint32_t& size, std::uint32_t& alignment) noexcept
{
    if (type == "float" || type == "int" || type == "uint" || type == "bool")
    {
        size      = 4U;
        alignment = 4U;
        return true;
    }

    std::string_view rest;
    std::uint32_t    components = 0U;
    if (type.substr(0U, 3U) == "vec")
    {
        rest = type.substr(3U);
    }
    else if (type.substr(0U, 4U) == "ivec" || type.substr(0U, 4U) == "uvec" || type.substr(0U, 4U) == "bvec")
    {
        rest = type.substr(4U);
    }
    if (!rest.empty() && rest.size() == 1U && rest[0] >= '2' && rest[0] <= '4')
    {
        components = static_cast<std::uint32_t>(rest[0] - '0');
        size       = components * 4U;
        alignment  = components == 2U ? 8U : 16U;
        return true;
    }

    if (type.substr(0U, 3U) == "mat")
    {
        // `matN` (N columns of vecN) or `matCxR` (C columns of vecR).
        std::string_view dims = type.substr(3U);
        std::uint32_t    columns = 0U;
        std::uint32_t    rows    = 0U;
        const std::size_t cross  = dims.find('x');
        if (cross == std::string_view::npos)
        {
            if (!parseUnsigned(dims, columns) || columns < 2U || columns > 4U)
            {
                return false;
            }
            rows = columns;
        }
        else
        {
            if (!parseUnsigned(dims.substr(0U, cross), columns) || !parseUnsigned(dims.substr(cross + 1U), rows) ||
                columns < 2U || columns > 4U || rows < 2U || rows > 4U)
            {
                return false;
            }
        }
        const std::uint32_t column_size = rows == 2U ? 8U : (rows == 3U ? 12U : 16U);
        size                            = columns * roundUp(column_size, 16U);
        alignment                       = 16U;
        return true;
    }

    return false;   // double / struct members and the rest: reported, never guessed at
}

/** @brief The size of one member (@p count elements when it is an array). */
bool memberLayout(std::string_view type, std::uint32_t count, bool std140, std::uint32_t& size,
                  std::uint32_t& alignment) noexcept
{
    std::uint32_t element_size = 0U;
    if (!memberTypeLayout(type, element_size, alignment))
    {
        return false;
    }
    const std::uint32_t stride = count > 1U ? roundUp(element_size, std140 ? 16U : alignment) : element_size;
    size                       = stride * count;
    return true;
}

/**
 * @brief Sizes one block body (the text between its braces).
 *
 * @param members  The block's members, semicolon separated.
 * @param std140   Whether the block declares the std140 layout (std430 rules otherwise, as push blocks have).
 * @param out      Receives the size in bytes.
 * @param recorded When not null, receives one entry per member (name, offset, size) - the push blocks'
 *                 serving layer fills them BY NAME, so the walk that already knows where each member sits is
 *                 also the place that records it.
 * @return true when every member was sized; false when a member type is outside the scan's table.
 */
bool sizeBlockMembers(std::string_view members, bool std140, std::uint32_t& out,
                      std::vector<AbiPushMember>* recorded = nullptr)
{
    std::uint32_t end       = 0U;
    std::uint32_t max_align = 1U;

    for (std::size_t at = 0U; at < members.size();)
    {
        const std::size_t semi = members.find(';', at);
        const std::string_view declaration =
            trim(members.substr(at, (semi == std::string_view::npos ? members.size() : semi) - at));
        at = semi == std::string_view::npos ? members.size() : semi + 1U;
        if (declaration.empty())
        {
            continue;
        }

        std::size_t      cursor = 0U;
        std::string_view type;
        if (!readWord(declaration, cursor, type))
        {
            return false;
        }
        for (;;)
        {
            std::string_view name;
            if (!readWord(declaration, cursor, name))
            {
                return false;
            }
            std::uint32_t      count = 1U;
            const std::size_t  probe = skipSpaces(declaration, cursor);
            if (probe < declaration.size() && declaration[probe] == '[')
            {
                const std::size_t close = declaration.find(']', probe);
                if (close == std::string_view::npos || !parseUnsigned(declaration.substr(probe + 1U, close - probe - 1U), count) ||
                    count == 0U)
                {
                    return false;
                }
                cursor = close + 1U;
            }
            else
            {
                cursor = probe;
            }

            std::uint32_t size      = 0U;
            std::uint32_t alignment = 0U;
            if (!memberLayout(type, count, std140, size, alignment))
            {
                return false;
            }
            end       = roundUp(end, alignment) + size;
            max_align = std::max(max_align, alignment);
            if (recorded != nullptr)
            {
                recorded->push_back(AbiPushMember{ std::string(name), end - size, size });
            }

            const std::size_t next = skipSpaces(declaration, cursor);
            if (next < declaration.size() && declaration[next] == ',')
            {
                cursor = next + 1U;
                continue;
            }
            if (next != declaration.size())
            {
                return false;   // trailing garbage: the scan does not know what this declaration is
            }
            break;
        }
    }

    const std::uint32_t block_align = std140 ? std::max<std::uint32_t>(16U, max_align) : max_align;
    out                             = roundUp(end, block_align);
    return true;
}

/** @brief The sampler kind a GLSL sampler type name is, and whether the name is a sampler at all. */
bool samplerKindOf(std::string_view type, AbiDescriptorKind& out) noexcept
{
    const bool is_sampler = type.substr(0U, 7U) == "sampler" || type.substr(0U, 8U) == "isampler" ||
                            type.substr(0U, 8U) == "usampler";
    if (!is_sampler)
    {
        return false;
    }
    if (type == "sampler2D")
    {
        out = AbiDescriptorKind::Sampler2D;
    }
    else if (type == "samplerCube")
    {
        out = AbiDescriptorKind::SamplerCube;
    }
    else
    {
        out = AbiDescriptorKind::OtherSampler;   // recorded by name; the serving layer decides
    }
    return true;
}

/**
 * @brief Reads one stage's declarations with the conditional lines already removed.
 *
 * @param text   The stage's taken lines.
 * @param stage  The stage (vertex or fragment).
 * @param out    Receives one entry per declared binding (unsorted).
 * @param pushes Receives one entry per declared push block, when the text declares one.
 * @return true when the text was read; false when a declaration form is outside this scan's grammar.
 */
bool scanStageDeclarations(std::string_view text, AbiStage stage, std::vector<AbiBinding>& out,
                           std::vector<AbiPushRange>& pushes)
{
    const std::uint32_t stage_bit = static_cast<std::uint32_t>(stage);

    for (std::size_t at = 0U; at < text.size();)
    {
        const std::size_t layout_at = text.find("layout", at);
        if (layout_at == std::string_view::npos)
        {
            break;
        }
        at = layout_at + 6U;
        if (layout_at != 0U && isNameChar(text[layout_at - 1U]))
        {
            continue;   // part of a longer word, not the qualifier keyword
        }

        const std::size_t open = skipSpaces(text, at);
        if (open >= text.size() || text[open] != '(')
        {
            continue;
        }
        const std::size_t close = text.find(')', open);
        if (close == std::string_view::npos)
        {
            return false;
        }
        const Qualifiers qualifiers = parseQualifiers(text.substr(open + 1U, close - open - 1U));
        if (!qualifiers.classified)
        {
            return false;
        }

        std::size_t      cursor = close + 1U;
        std::string_view keyword;
        if (!readWord(text, cursor, keyword) || keyword != "uniform")
        {
            continue;   // `in` / `out` / `buffer` / a `local_size` list: no descriptor binding
        }
        std::string_view type;
        if (!readWord(text, cursor, type))
        {
            return false;
        }

        const std::size_t body = skipSpaces(text, cursor);
        if (body < text.size() && text[body] == '{')
        {
            std::size_t depth = 0U;
            std::size_t end   = std::string_view::npos;
            for (std::size_t probe = body; probe < text.size(); ++probe)
            {
                if (text[probe] == '{')
                {
                    ++depth;
                }
                else if (text[probe] == '}')
                {
                    if (--depth == 0U)
                    {
                        end = probe;
                        break;
                    }
                }
            }
            if (end == std::string_view::npos)
            {
                return false;
            }

            // `}`, then an optional instance name, an optional array suffix and a `;`.
            std::size_t      tail = end + 1U;
            std::string_view instance;
            std::uint32_t    count = 1U;
            if (readWord(text, tail, instance))
            {
                const std::size_t probe = skipSpaces(text, tail);
                if (probe < text.size() && text[probe] == '[')
                {
                    const std::size_t array_close = text.find(']', probe);
                    if (array_close == std::string_view::npos ||
                        !parseUnsigned(text.substr(probe + 1U, array_close - probe - 1U), count) || count == 0U)
                    {
                        return false;
                    }
                    tail = array_close + 1U;
                }
                else
                {
                    tail = probe;
                }
            }
            else
            {
                tail = skipSpaces(text, tail);
            }
            if (tail < text.size() && text[tail] == ';')
            {
                ++tail;
            }
            at = tail;

            const bool std140 = qualifiers.layout == AbiBlockLayout::Std140;
            std::uint32_t    block_size = 0U;
            // A push block's members are recorded as well (a push range is ASSEMBLED from named values, see
            // AbiPushRange); a uniform block's are not - its bytes come from an L1 struct whole.
            std::vector<AbiPushMember> push_members;
            // A push block's layout is std430 unless the text says std140. A UNIFORM block without a layout
            // qualifier is `shared` (the compiler's own layout), so its size is not this scan's to compute:
            // the fact is 0 = "not known here", and only a declared std140 gets a size (see ProgramAbi).
            const bool       sized = qualifiers.push_constant
                                         ? sizeBlockMembers(text.substr(body + 1U, end - body - 1U), std140, block_size,
                                                            &push_members)
                                         : (std140 && sizeBlockMembers(text.substr(body + 1U, end - body - 1U), true, block_size));

            if (qualifiers.push_constant)
            {
                AbiPushRange range;
                range.offset    = qualifiers.has_offset ? qualifiers.offset : 0U;
                range.size      = sized ? block_size : 0U;
                range.stages    = stage_bit;
                range.type_name = std::string(type);
                if (sized)
                {
                    range.members = std::move(push_members);
                }
                pushes.push_back(std::move(range));
                continue;
            }
            if (!qualifiers.has_binding)
            {
                continue;   // a block with no binding qualifier: the compiler chooses one, not this text
            }

            AbiBinding binding;
            binding.set        = qualifiers.has_set ? qualifiers.set : 0U;
            binding.binding    = qualifiers.binding;
            binding.count      = count;
            binding.kind       = AbiDescriptorKind::UniformBlock;
            binding.role       = blockRoleOf(type);
            binding.stages     = stage_bit;
            binding.layout     = qualifiers.layout;
            binding.block_size = sized ? block_size : 0U;
            binding.type_name  = std::string(type);
            binding.name       = std::string(instance);
            out.push_back(std::move(binding));
            continue;
        }

        // An opaque declaration: `uniform sampler2D name;` (and the sampler species this scan names).
        AbiDescriptorKind kind = AbiDescriptorKind::UniformBlock;
        if (!samplerKindOf(type, kind))
        {
            continue;   // `uniform Foo foo;`: not a binding this backend serves
        }
        std::size_t      tail = cursor;
        std::string_view name;
        if (!readWord(text, tail, name))
        {
            return false;
        }
        std::uint32_t count = 1U;
        std::size_t   probe = skipSpaces(text, tail);
        if (probe < text.size() && text[probe] == '[')
        {
            const std::size_t array_close = text.find(']', probe);
            if (array_close == std::string_view::npos ||
                !parseUnsigned(text.substr(probe + 1U, array_close - probe - 1U), count) || count == 0U)
            {
                return false;
            }
            probe = array_close + 1U;
        }
        at = probe;
        if (!qualifiers.has_binding)
        {
            continue;   // no binding qualifier: the compiler assigns one, so the text states nothing
        }

        AbiBinding binding;
        binding.set       = qualifiers.has_set ? qualifiers.set : 0U;
        binding.binding   = qualifiers.binding;
        binding.count     = count;
        binding.kind      = kind;
        binding.role      = AbiBlockRole::NotABlock;
        binding.stages    = stage_bit;
        binding.type_name = std::string(type);
        binding.name      = std::string(name);
        out.push_back(std::move(binding));
    }
    return true;
}

/** @brief Whether two declarations of one (set, binding) say the same thing. */
bool sameDeclaration(const AbiBinding& left, const AbiBinding& right) noexcept
{
    return left.kind == right.kind && left.role == right.role && left.layout == right.layout &&
           left.block_size == right.block_size && left.count == right.count && left.type_name == right.type_name;
}

}  // namespace

AbiBlockRole blockRoleOf(std::string_view type_name) noexcept
{
    // The GLSL block type name IS the L1 name (graphics-shader.md §11.3), which is what lets a program put
    // the blocks wherever it likes and still be served.
    if (type_name == "VineViewBlock")
    {
        return AbiBlockRole::View;
    }
    if (type_name == "VineDrawBlock")
    {
        return AbiBlockRole::Draw;
    }
    if (type_name == "VineMaterialBlock")
    {
        return AbiBlockRole::Material;
    }
    if (type_name == "VineLightsBlock")
    {
        return AbiBlockRole::Lights;
    }
    if (type_name == "VineShadowBlock")
    {
        return AbiBlockRole::ShadowBlock;
    }
    return AbiBlockRole::Foreign;
}

const char* abiBlockRoleName(AbiBlockRole role) noexcept
{
    switch (role)
    {
    case AbiBlockRole::NotABlock: return "sampler";
    case AbiBlockRole::View: return "VineViewBlock";
    case AbiBlockRole::Draw: return "VineDrawBlock";
    case AbiBlockRole::Material: return "VineMaterialBlock";
    case AbiBlockRole::Lights: return "VineLightsBlock";
    case AbiBlockRole::ShadowBlock: return "VineShadowBlock";
    case AbiBlockRole::Foreign: return "foreign block";
    }
    return "unknown";
}

FactMiss scanProgramAbi(std::string_view vertex, std::string_view fragment,
                        std::span<const std::string_view> defines, ProgramAbi& out)
{
    out = ProgramAbi{};

    std::unordered_set<std::string> requested;
    for (const std::string_view define : defines)
    {
        if (!define.empty())
        {
            requested.insert(std::string(define));
        }
    }

    std::vector<AbiBinding>  declared;
    std::vector<AbiPushRange> pushes;

    const std::pair<std::string_view, AbiStage> stages[] = { { vertex, AbiStage::Vertex },
                                                             { fragment, AbiStage::Fragment } };
    for (const auto& [text, stage] : stages)
    {
        const std::string        source = stripComments(text);
        const ConditionalResult  conditional = applyConditionals(source, requested);
        switch (conditional.outcome)
        {
        case ConditionalResult::Outcome::Unclassifiable:
            return FactMiss::Malformed;
        case ConditionalResult::Outcome::Refused:
            // A `#error` in a taken branch: under THESE defines the source says it cannot compile, which is
            // the one thing a scan can see that a driver would only say later (and the SDK's programs use it
            // exactly this way, for a texcoord slot that was sampled without its kind).
            return FactMiss::Malformed;
        case ConditionalResult::Outcome::Ok:
            break;
        }
        out.import_defines.insert(out.import_defines.end(), conditional.import_defines.begin(),
                                  conditional.import_defines.end());
        if (!scanStageDeclarations(conditional.active, stage, declared, pushes))
        {
            return FactMiss::Malformed;
        }
    }

    // One binding may be declared by both stages (the same block read by the vertex and the fragment stage):
    // that is ONE binding whose stage flags are the union. Two declarations that disagree are not - the same
    // (set, binding) cannot be two different things, whatever the two stages call their instances.
    for (AbiBinding& binding : declared)
    {
        const auto found = std::find_if(out.bindings.begin(), out.bindings.end(), [&](const AbiBinding& entry) {
            return entry.set == binding.set && entry.binding == binding.binding;
        });
        if (found == out.bindings.end())
        {
            out.bindings.push_back(binding);
            continue;
        }
        if (!sameDeclaration(*found, binding))
        {
            return FactMiss::Malformed;   // one (set, binding) cannot be two different things
        }
        found->stages |= binding.stages;
    }
    for (const AbiPushRange& push : pushes)
    {
        const auto found = std::find_if(out.pushes.begin(), out.pushes.end(), [&](const AbiPushRange& entry) {
            return entry.offset == push.offset;
        });
        if (found == out.pushes.end())
        {
            out.pushes.push_back(push);
            continue;
        }
        // Two stages may declare ONE range (the same camera matrices read by both) - that is one range whose
        // stages are the union. Two ranges at one offset that differ in size OR in what their members are
        // called are a contradiction: the bytes cannot be two things.
        bool same = found->size == push.size && found->members.size() == push.members.size();
        for (std::size_t index = 0U; same && index < push.members.size(); ++index)
        {
            same = found->members[index].name == push.members[index].name &&
                   found->members[index].offset == push.members[index].offset &&
                   found->members[index].size == push.members[index].size;
        }
        if (!same)
        {
            return FactMiss::Malformed;
        }
        found->stages |= push.stages;
    }

    std::sort(out.bindings.begin(), out.bindings.end(), [](const AbiBinding& left, const AbiBinding& right) {
        return left.set != right.set ? left.set < right.set : left.binding < right.binding;
    });
    std::sort(out.pushes.begin(), out.pushes.end(), [](const AbiPushRange& left, const AbiPushRange& right) {
        return left.offset != right.offset ? left.offset < right.offset : left.size < right.size;
    });
    std::sort(out.import_defines.begin(), out.import_defines.end());
    out.import_defines.erase(std::unique(out.import_defines.begin(), out.import_defines.end()),
                             out.import_defines.end());
    return FactMiss::None;
}

VN_VSG_NS_END
