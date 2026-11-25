#include <ludus/audio/content/music_score.h>
#include <ludus/content/json.h>
#include <ludus/foundation/base/checked_integer.hpp>

#include <limits>

namespace ludus::audio::content
{
namespace
{
using ludus::content::JsonValue;
constexpr uint64 MUSIC_VERSION = 2;
constexpr uint32 MAX_TEMPO_TERM = 65535;
constexpr uint32 MAX_METER_TERM = 32;
constexpr uint64 SECONDS_PER_MINUTE = 60;
constexpr uint64 QUARTER_NOTE_UNIT = 4;

ContentStatus Reject(std::string_view field, Diagnostic& outDiagnostic) noexcept
{
    outDiagnostic = {};
    outDiagnostic.Result = ContentStatus::Invalid;
    field.copy(outDiagnostic.Field, sizeof(outDiagnostic.Field) - 1);
    return outDiagnostic.Result;
}
bool ReadId(JsonValue value, ResourceId& outId) noexcept
{
    std::string_view text;
    return value.String(text) == true && ludus::content::ValidId(text) == true && outId.Set(text) == true;
}
bool ReadUnit(JsonValue value, float32& outValue) noexcept
{
    float64 number = 0;
    if (value.Number(number) == false || number < 0 || number > 1)
    {
        return false;
    }
    outValue = static_cast<float32>(number);
    return true;
}
template <class Integer>
bool ReadInteger(JsonValue value, uint64 minimum, uint64 maximum, Integer& outValue) noexcept
{
    uint64 number = 0;
    if (value.Integer(number) == false || number < minimum || number > maximum)
    {
        return false;
    }
    outValue = static_cast<Integer>(number);
    return true;
}
bool ReadBoundary(JsonValue value, MusicBoundary& outBoundary) noexcept
{
    std::string_view text;
    if (value.String(text) == false || (text != "beat" && text != "bar"))
    {
        return false;
    }
    outBoundary = text == "beat" ? MusicBoundary::Beat : MusicBoundary::Bar;
    return true;
}
template <class Item, usize Capacity, class Reader>
bool ReadRows(JsonValue value,
              ludus::foundation::StaticArray<Item, Capacity>& outItems,
              usize& outCount,
              Reader read) noexcept
{
    if (value.Array() == false || value.Count() > Capacity)
    {
        return false;
    }
    outCount = value.Count();
    for (usize i = 0; i < outCount; ++i)
    {
        if (read(value.At(i), outItems[i]) == false)
        {
            return false;
        }
    }
    return true;
}
bool ReadLayer(JsonValue value, MusicLayer& outLayer) noexcept
{
    const auto readPoint = [](JsonValue point, LayerPoint& outPoint) {
        return point.Array() == true && point.Count() == 2 && ReadUnit(point.At(0), outPoint.Intensity) == true &&
               ReadUnit(point.At(1), outPoint.Gain) == true;
    };
    if (value.Fields({"source", "gain_curve"}) == false || ReadId(value.Get("source"), outLayer.Source) == false ||
        ReadRows(value.Get("gain_curve"), outLayer.Points, outLayer.PointCount, readPoint) == false ||
        outLayer.PointCount < 2 || outLayer.Points[0].Intensity != 0 ||
        outLayer.Points[outLayer.PointCount - 1].Intensity != 1)
    {
        return false;
    }
    for (usize i = 1; i < outLayer.PointCount; ++i)
    {
        if (outLayer.Points[i - 1].Intensity >= outLayer.Points[i].Intensity)
        {
            return false;
        }
    }
    return true;
}
bool ReadSection(JsonValue value, MusicSection& outSection) noexcept
{
    return value.Fields({"id", "loop_begin_frame", "loop_end_frame", "layers"}) == true &&
           ReadId(value.Get("id"), outSection.Id) == true &&
           value.Get("loop_begin_frame").Integer(outSection.LoopBegin) == true &&
           value.Get("loop_end_frame").Integer(outSection.LoopEnd) == true &&
           outSection.LoopBegin < outSection.LoopEnd &&
           ReadRows(value.Get("layers"), outSection.Layers, outSection.LayerCount, ReadLayer) == true &&
           outSection.LayerCount > 0;
}
template <class Item, usize Capacity>
usize FindId(const ludus::foundation::StaticArray<Item, Capacity>& items, usize count, const ResourceId& id) noexcept
{
    for (usize i = 0; i < count; ++i)
    {
        if (items[i].Id.View() == id.View())
        {
            return i;
        }
    }
    return count;
}
template <class Item, usize Capacity>
bool HasUniqueIds(const ludus::foundation::StaticArray<Item, Capacity>& items, usize count) noexcept
{
    for (usize i = 0; i < count; ++i)
    {
        if (FindId(items, i, items[i].Id) != i)
        {
            return false;
        }
    }
    return true;
}
bool ReadTransition(JsonValue value, MusicTransition& outEdge) noexcept
{
    return value.Fields({"from", "to", "quantize", "fade_frames"}) == true &&
           ReadId(value.Get("from"), outEdge.From) == true && ReadId(value.Get("to"), outEdge.To) == true &&
           ReadBoundary(value.Get("quantize"), outEdge.Quantize) == true &&
           ReadInteger(value.Get("fade_frames"), 0, std::numeric_limits<uint32>::max(), outEdge.FadeFrames) == true;
}
bool ValidateEdges(const MusicScore& score) noexcept
{
    for (usize i = 0; i < score.TransitionCount; ++i)
    {
        const MusicTransition& edge = score.Transitions[i];
        const usize from = FindId(score.Sections, score.SectionCount, edge.From);
        const usize to = FindId(score.Sections, score.SectionCount, edge.To);
        if (from == to || from == score.SectionCount || to == score.SectionCount ||
            score.Sections[from].LayerCount + score.Sections[to].LayerCount > STREAM_CAPACITY)
        {
            return false;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (edge.From.View() == score.Transitions[j].From.View() &&
                edge.To.View() == score.Transitions[j].To.View())
            {
                return false;
            }
        }
    }
    return true;
}
struct DivisionResult final
{
    uint64 Whole = 0;
    uint64 Remainder = 0;
};
bool MultiplyDivide(uint64 value, uint64 multiplier, uint64 divisor, DivisionResult& outResult) noexcept
{
    uint64 fraction = 0;
    uint64 whole = 0;
    if (TryMultiply(value / divisor, multiplier, whole) == false ||
        TryMultiply(value % divisor, multiplier, fraction) == false ||
        TryAdd(whole, fraction / divisor, outResult.Whole) == false)
    {
        return false;
    }
    outResult.Remainder = fraction % divisor;
    return true;
}
} // namespace

bool IsDynamicMusicDocument(std::string_view json) noexcept
{
    ludus::content::JsonDocument document;
    Diagnostic diagnostic;
    uint64 version = 0;
    return document.Read(json, diagnostic) == ContentStatus::Ok &&
           document.Root().Get("version").Integer(version) == true && version == MUSIC_VERSION;
}

ContentStatus ReadMusicScore(std::string_view json, MusicScore& outScore, Diagnostic& outDiagnostic) noexcept
{
    ludus::content::JsonDocument document;
    const ContentStatus status = document.Read(json, outDiagnostic);
    if (status != ContentStatus::Ok)
    {
        return status;
    }
    const JsonValue root = document.Root();
    MusicScore score;
    uint64 version = 0;
    if (root.Fields({"version",
                     "id",
                     "bus",
                     "priority",
                     "gain",
                     "tempo",
                     "meter",
                     "initial_state",
                     "states",
                     "sections",
                     "transitions",
                     "layer_ramp_frames",
                     "max_stingers",
                     "stingers"}) == false ||
        root.Get("version").Integer(version) == false || version != MUSIC_VERSION ||
        ReadId(root.Get("id"), score.Id) == false || ReadId(root.Get("bus"), score.Bus) == false ||
        ReadInteger(root.Get("priority"), 0, PRIORITY_MAX, score.Priority) == false ||
        ReadUnit(root.Get("gain"), score.Gain) == false ||
        ReadId(root.Get("initial_state"), score.InitialState) == false ||
        ReadInteger(root.Get("layer_ramp_frames"), 0, std::numeric_limits<uint32>::max(), score.LayerRampFrames) ==
            false ||
        ReadInteger(root.Get("max_stingers"), 0, MAX_MUSIC_STINGERS, score.MaxStingers) == false)
    {
        return Reject("music.v2.header", outDiagnostic);
    }
    const JsonValue tempo = root.Get("tempo");
    const JsonValue meter = root.Get("meter");
    if (tempo.Fields({"numerator", "denominator"}) == false || meter.Fields({"beats", "unit"}) == false ||
        ReadInteger(tempo.Get("numerator"), 1, MAX_TEMPO_TERM, score.TempoNumerator) == false ||
        ReadInteger(tempo.Get("denominator"), 1, MAX_TEMPO_TERM, score.TempoDenominator) == false ||
        ReadInteger(meter.Get("beats"), 1, MAX_METER_TERM, score.MeterBeats) == false ||
        ReadInteger(meter.Get("unit"), 1, MAX_METER_TERM, score.MeterUnit) == false ||
        (score.MeterUnit & (score.MeterUnit - 1)) != 0)
    {
        return Reject("music.v2.tempo/meter", outDiagnostic);
    }
    if (ReadRows(root.Get("sections"), score.Sections, score.SectionCount, ReadSection) == false ||
        score.SectionCount == 0 || HasUniqueIds(score.Sections, score.SectionCount) == false)
    {
        return Reject("music.v2.sections", outDiagnostic);
    }
    const auto readState = [&](JsonValue value, MusicState& outState) {
        return value.Fields({"id", "section"}) == true && ReadId(value.Get("id"), outState.Id) == true &&
               ReadId(value.Get("section"), outState.Section) == true &&
               FindId(score.Sections, score.SectionCount, outState.Section) < score.SectionCount;
    };
    if (ReadRows(root.Get("states"), score.States, score.StateCount, readState) == false ||
        HasUniqueIds(score.States, score.StateCount) == false ||
        FindId(score.States, score.StateCount, score.InitialState) == score.StateCount)
    {
        return Reject("music.v2.states", outDiagnostic);
    }
    if (ReadRows(root.Get("transitions"), score.Transitions, score.TransitionCount, ReadTransition) == false ||
        ValidateEdges(score) == false)
    {
        return Reject("music.v2.transitions/capacity", outDiagnostic);
    }
    const auto readStinger = [](JsonValue value, MusicStinger& outStinger) {
        return value.Fields({"id", "sound", "quantize"}) == true && ReadId(value.Get("id"), outStinger.Id) == true &&
               ReadId(value.Get("sound"), outStinger.Sound) == true &&
               ReadBoundary(value.Get("quantize"), outStinger.Quantize) == true;
    };
    if (ReadRows(root.Get("stingers"), score.Stingers, score.StingerCount, readStinger) == false ||
        HasUniqueIds(score.Stingers, score.StingerCount) == false ||
        (score.MaxStingers == 0) != (score.StingerCount == 0))
    {
        return Reject("music.v2.stingers", outDiagnostic);
    }
    outScore = score;
    outDiagnostic = {};
    return ContentStatus::Ok;
}
bool QuantizeMusicFrame(const MusicScore& score,
                        uint32 sampleRate,
                        MusicBoundary boundary,
                        uint64 origin,
                        uint64 requestedFrame,
                        uint64& outFrame) noexcept
{
    if (sampleRate == 0 || score.TempoNumerator == 0 || score.TempoDenominator == 0 || score.MeterBeats == 0 ||
        score.MeterUnit == 0 || (boundary != MusicBoundary::Beat && boundary != MusicBoundary::Bar))
    {
        return false;
    }
    uint64 period = SECONDS_PER_MINUTE * QUARTER_NOTE_UNIT * sampleRate;
    uint64 divisor = uint64{score.TempoNumerator} * score.MeterUnit;
    if (TryMultiply(period, uint64{score.TempoDenominator}, period) == false ||
        (boundary == MusicBoundary::Bar && TryMultiply(period, uint64{score.MeterBeats}, period) == false))
    {
        return false;
    }
    if (requestedFrame <= origin)
    {
        outFrame = origin;
        return true;
    }
    // Reduce the frame period before multiplying by an absolute beat index.
    uint64 common = period;
    uint64 remaining = divisor;
    while (remaining != 0)
    {
        const uint64 next = common % remaining;
        common = remaining;
        remaining = next;
    }
    period /= common;
    divisor /= common;
    uint64 index = 0;
    DivisionResult division;
    uint64 offset = 0;
    if (MultiplyDivide(requestedFrame - origin - 1, divisor, period, division) == false ||
        TryAdd(division.Whole, uint64{1}, index) == false ||
        MultiplyDivide(index, period, divisor, division) == false ||
        TryAdd(division.Whole, uint64{division.Remainder != 0}, offset) == false)
    {
        return false;
    }
    return TryAdd(origin, offset, outFrame);
}
} // namespace ludus::audio::content
