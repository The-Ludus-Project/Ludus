#include "formatter_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace
{
using ludus::tooling::shader_language::Status;
using ludus::tooling::shader_language::test::FormatText;
using ludus::tooling::shader_language::test::SpellTokens;

/// Format `input`, require `expected`, and check that only whitespace changed and that the result
/// is a fixed point.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): input then expected, as in CHECK.
void RequireFormatted(std::string_view input, std::string_view expected)
{
    Status status = Status::InvalidRequest;
    const std::string formatted = FormatText(input, status);
    INFO("input:\n" << input << "\nformatted:\n" << formatted);
    REQUIRE(status == Status::Ready);
    CHECK(formatted == expected);
    CHECK(SpellTokens(formatted) == SpellTokens(input));
    Status again = Status::InvalidRequest;
    CHECK(FormatText(formatted, again) == formatted);
    CHECK(again == Status::Ready);
}

constexpr std::string_view MESSY = R"(#pragma once
#include "common.luslh"
#if MODE == 1
#define SCALE(x) ((x) * 2)
#else
#  define SCALE(x) (x)
#endif
static const uint32 COUNT=4;   // number of taps
struct Tap{float32 Weight;float32x2 OffsetPixel = 0;};
[stage(compute),threads(8,8,1)]
void Main(in uint32x3 id:DispatchThreadId){
float32 sum=0;


for(uint32 i=0;i<COUNT;i++){sum+=-Weights[i]*SCALE(i);}
if(sum>1&&id.x==0){sum=1;}else{sum=0;}
switch(mode){case Mode::Off: sum = 0; break;
default: break;}
Texture2D<float32x4> t;
Hit hit={.PositionWorld=p,.Count=1};
Trace(scene,ray=r,outRadiance=out radiance);
do{sum-=1;}while(sum>0);
/* block
   comment */
}
)";

constexpr std::string_view CANONICAL = R"(#pragma once
#include "common.luslh"
#if MODE == 1
#    define SCALE(x) ((x) * 2)
#else
#    define SCALE(x) (x)
#endif
static const uint32 COUNT = 4; // number of taps
struct Tap
{
    float32 Weight;
    float32x2 OffsetPixel = 0;
};
[stage(compute), threads(8, 8, 1)]
void Main(in uint32x3 id : DispatchThreadId)
{
    float32 sum = 0;

    for (uint32 i = 0; i < COUNT; i++)
    {
        sum += -Weights[i] * SCALE(i);
    }
    if (sum > 1 && id.x == 0)
    {
        sum = 1;
    }
    else
    {
        sum = 0;
    }
    switch (mode)
    {
        case Mode::Off:
            sum = 0;
            break;
        default:
            break;
    }
    Texture2D<float32x4> t;
    Hit hit = { .PositionWorld = p, .Count = 1 };
    Trace(scene, ray = r, outRadiance = out radiance);
    do
    {
        sum -= 1;
    } while (sum > 0);
    /* block
   comment */
}
)";
} // namespace

TEST_CASE("LUSL formatter produces the canonical layout", "[shader-language][formatter]")
{
    RequireFormatted(MESSY, CANONICAL);
    RequireFormatted(CANONICAL, CANONICAL);
}

TEST_CASE("LUSL formatter aligns continued lines and keeps author line breaks", "[shader-language][formatter]")
{
    RequireFormatted("float32 Mix(in float32 a,\nin float32 b);\n",
                     "float32 Mix(in float32 a,\n            in float32 b);\n");
    RequireFormatted("float32 Mix(\nin float32 a, in float32 b);\n",
                     "float32 Mix(\n    in float32 a, in float32 b);\n");
    RequireFormatted("void F()\n{\nx = a +\nb;\nif (x)\nreturn;\n}\n",
                     "void F()\n{\n    x = a +\n        b;\n    if (x)\n        return;\n}\n");
    RequireFormatted("void F() { a = 1; b = 2; }", "void F()\n{\n    a = 1;\n    b = 2;\n}\n");
}

TEST_CASE("LUSL formatter spaces operators by role", "[shader-language][formatter]")
{
    RequireFormatted("x=-y*~z-(+w);", "x = -y * ~z - (+w);\n");
    RequireFormatted("return -a;", "return -a;\n");
    RequireFormatted("x=a[i]-b(c)-d.e;", "x = a[i] - b(c) - d.e;\n");
    RequireFormatted("x=A::B;y=float32x3(1.0);", "x = A::B;\ny = float32x3(1.0);\n");
    RequireFormatted("x=y<z;w=c?a:b;", "x = y < z;\nw = c ? a : b;\n");
    RequireFormatted("Outer<Inner<float32>> q;", "Outer<Inner<float32>> q;\n");
    RequireFormatted("template<Material M> float32 F();", "template <Material M> float32 F();\n");
    RequireFormatted("Hit h={};", "Hit h = {};\n");
    RequireFormatted("[[fallthrough]];", "[[fallthrough]];\n");
    RequireFormatted("i ++;-- j;", "i++;\n--j;\n");
}

TEST_CASE("LUSL formatter keeps comments, blank lines and directives", "[shader-language][formatter]")
{
    RequireFormatted("// header\n\n\n\nx = 1; /* a */ // b\n", "// header\n\nx = 1; /* a */ // b\n");
    RequireFormatted("#if A\n#if B\n#define X 1\n#endif\n#endif\n",
                     "#if A\n#    if B\n#        define X 1\n#    endif\n#endif\n");
    RequireFormatted("#define LONG(a) a + \\\n    a\nx = LONG(1);\n", "#define LONG(a) a + \\\n    a\nx = LONG(1);\n");
    RequireFormatted("void F()\n{\n\n    x = 1;\n\n}\n", "void F()\n{\n    x = 1;\n}\n");
    RequireFormatted("", "");
}

TEST_CASE("LUSL formatter survives a case label left open across its switch", "[shader-language][formatter]")
{
    // Found by the front-end fuzzer: the label's ':' arrives after the switch body has closed.
    Status status = Status::InvalidRequest;
    const std::string_view source = "switch (x) { case } : y;\ncase ; = } ;\n";
    const std::string formatted = FormatText(source, status);
    CHECK(status == Status::Ready);
    CHECK(SpellTokens(formatted) == SpellTokens(source));
    Status again = Status::InvalidRequest;
    CHECK(FormatText(formatted, again) == formatted);
}

TEST_CASE("LUSL formatter rejects sources it cannot lex", "[shader-language][formatter]")
{
    Status status = Status::Ready;
    CHECK(FormatText("x = 1; /* open", status).empty() == true);
    CHECK(status == Status::Failed);
    CHECK(FormatText("x = \"open;\n", status).empty() == true);
    CHECK(status == Status::Failed);
    CHECK(FormatText(std::string_view("a\0b", 3), status).empty() == true);
    CHECK(status == Status::InvalidRequest);
    CHECK(FormatText("x = \xff;", status).empty() == true);
    CHECK(status == Status::InvalidRequest);
}
