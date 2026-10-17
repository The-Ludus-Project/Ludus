#include <ludus/foundation/config/config.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::foundation::config;
namespace
{
// Test fixture data: its string_view members are built from literals, which
// cannot fail, although the converting constructor is not declared noexcept.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
Descriptor Schema[]{
    {
        .Name = "render.width",
        .Default = Value::FromUint32(800),
        .Help = {},
        .Units = {},
        .Choices = {},
        .MinUnsigned = 64,
        .MaxUnsigned = 8192,
        .Persistent = true,
    },
    {
        .Name = "render.height",
        .Default = Value::FromUint32(600),
        .Help = {},
        .Units = {},
        .Choices = {},
        .MinUnsigned = 64,
        .MaxUnsigned = 8192,
        .Persistent = true,
    },
    {
        .Name = "device.backend",
        .Default = Value::FromBool(false),
        .Help = {},
        .Units = {},
        .Choices = {},
        .Group = 1,
        .Application = Apply::RestartEngine,
    },
    {
        .Name = "simulation.fixed",
        .Default = Value::FromBool(true),
        .Help = {},
        .Units = {},
        .Choices = {},
        .Group = 2,
        .Simulation = true,
    },
};
Explanation Explain(const Context& context, std::string_view name)
{
    Binding binding;
    Explanation explanation;
    REQUIRE(context.Bind(name, binding) == Status::Ok);
    REQUIRE(context.Explain(binding, explanation) == Status::Ok);
    return explanation;
}
Status PairConstraint(void*, std::span<const Value> values, Diagnostic&) noexcept
{
    return values[0].Unsigned * values[1].Unsigned <= 4000000 ? Status::Ok : Status::ConstraintFailed;
}
struct FaultDomain
{
    usize Calls = 0, FailAt = 0, Live = 0;
    static void* Allocate(void* data, usize bytes, usize alignment) noexcept
    {
        auto& self = *static_cast<FaultDomain*>(data);
        if (self.Calls++ == self.FailAt)
        {
            return nullptr;
        }
        void* allocation = GetSystemAllocationDomain().TryAllocate(bytes, alignment);
        self.Live += allocation == nullptr ? 0U : 1U;
        return allocation;
    }
    static void Free(void* data, void* allocation, usize bytes, usize alignment) noexcept
    {
        static_cast<FaultDomain*>(data)->Release(allocation, bytes, alignment);
    }
    void Release(void* allocation, usize bytes, usize alignment) noexcept
    {
        --Live;
        GetSystemAllocationDomain().Free(allocation, bytes, alignment);
    }
};
} // namespace
TEST_CASE("Config precedence, provenance, reset, and generation are independent of edit time")
{
    Context context;
    Diagnostic error;
    REQUIRE(context.Initialize(Schema, GetSystemAllocationDomain(), error) == Status::Ok);
    const Assignment launch[]{{ .Name = "render.width", .Data = Value::FromUint32(1024) }};
    REQUIRE(context.Prepare(Layer::Launch, launch, false, context.Revision(), error) == Status::Ok);
    REQUIRE(context.Commit(context.Revision(), error) == Status::Ok);
    const auto generation = Explain(context, "render.width").GroupGeneration;
    const Assignment preference[]{{ .Name = "render.width", .Data = Value::FromUint32(900) }};
    REQUIRE(context.Prepare(Layer::Preference, preference, true, context.Revision(), error) == Status::Ok);
    REQUIRE(context.Commit(context.Revision(), error) == Status::Ok);
    auto result = Explain(context, "render.width");
    CHECK(result.Requested.Unsigned == 1024);
    CHECK(result.Winner == Layer::Launch);
    CHECK(result.Overridden == 1);
    CHECK(result.GroupGeneration == generation);
    const Assignment reset[]{{ .Name = "render.width", .Remove = true }};
    REQUIRE(context.Prepare(Layer::Launch, reset, false, context.Revision(), error) == Status::Ok);
    REQUIRE(context.Commit(context.Revision(), error) == Status::Ok);
    CHECK(Explain(context, "render.width").Requested.Unsigned == 900);
    REQUIRE(context.Prepare(Layer::Preference, {}, true, context.Revision(), error) == Status::Ok);
    REQUIRE(context.Commit(context.Revision(), error) == Status::Ok);
    CHECK(Explain(context, "render.width").IsDefault);
}
TEST_CASE("Config invalid and stale transactions preserve committed values")
{
    Context context;
    Diagnostic error;
    REQUIRE(context.Initialize(Schema, GetSystemAllocationDomain(), error, {}, PairConstraint) == Status::Ok);
    const auto revision = context.Revision();
    const Assignment bad[]{{ .Name = "render.width", .Data = Value::FromUint32(8000) },
                           { .Name = "render.height", .Data = Value::FromUint32(8000) }};
    CHECK(context.Prepare(Layer::Project, bad, false, revision, error) == Status::ConstraintFailed);
    CHECK(context.Revision() == revision);
    CHECK(Explain(context, "render.width").Active.Unsigned == 800);
    const Assignment wrong[]{{ .Name = "render.width", .Data = Value::FromBool(true) }};
    CHECK(context.Prepare(Layer::Project, wrong, false, revision, error) == Status::InvalidValue);
    const Assignment duplicate[]{{ .Name = "render.width", .Data = Value::FromUint32(900) },
                                 { .Name = "render.width", .Data = Value::FromUint32(900) }};
    CHECK(context.Prepare(Layer::Project, duplicate, false, revision, error) == Status::DuplicateAssignment);
    CHECK(context.Prepare(Layer::Project, {}, true, revision - 1, error) == Status::Conflict);
    REQUIRE(context.Prepare(Layer::Project, {}, true, revision, error) == Status::Ok);
    CHECK(context.Commit(revision - 1, error) == Status::Conflict);
    CHECK(context.Revision() == revision);
    context.Discard();
    CHECK(context.Commit(revision, error) == Status::InvalidState);
    Context other;
    REQUIRE(other.Initialize(Schema, GetSystemAllocationDomain(), error) == Status::Ok);
    Binding binding;
    Explanation output;
    REQUIRE(context.Bind("render.width", binding) == Status::Ok);
    CHECK(other.Explain(binding, output) == Status::InvalidState);
}
TEST_CASE("Config restart state, simulation freeze, source policy, and apply groups")
{
    Context context;
    Diagnostic error;
    REQUIRE(context.Initialize(Schema, GetSystemAllocationDomain(), error) == Status::Ok);
    REQUIRE(context.SealStartup() == Status::Ok);
    const Assignment restart[]{{ .Name = "device.backend", .Data = Value::FromBool(true) }};
    REQUIRE(context.Prepare(Layer::Session, restart, false, context.Revision(), error) == Status::Ok);
    REQUIRE(context.Commit(context.Revision(), error) == Status::PendingRestart);
    CHECK(Explain(context, "device.backend").Pending);
    CHECK_FALSE(Explain(context, "device.backend").Active.Boolean);
    const Assignment frozen[]{{ .Name = "simulation.fixed", .Data = Value::FromBool(false) }};
    CHECK(context.Prepare(Layer::Session, frozen, false, context.Revision(), error) == Status::Frozen);
    const Assignment mixed[]{{ .Name = "render.width", .Data = Value::FromUint32(900) },
                             { .Name = "device.backend", .Data = Value::FromBool(false) }};
    CHECK(context.Prepare(Layer::Session, mixed, false, context.Revision(), error) == Status::MixedApplyGroups);
    CHECK(context.Prepare(Layer::Preference, restart, false, context.Revision(), error) == Status::InvalidSource);
}
TEST_CASE("Config all initialization allocation failures are recoverable and commits allocate nothing")
{
    for (usize failure = 0; failure < 7; ++failure)
    {
        FaultDomain fault;
        fault.FailAt = failure;
        AllocationDomain domain(&fault, FaultDomain::Allocate, FaultDomain::Free);
        {
            Context context;
            Diagnostic error;
            CHECK(context.Initialize(Schema, domain, error) == Status::OutOfMemory);
            CHECK_FALSE(context.IsValid());
        }
        CHECK(fault.Live == 0);
    }
    FaultDomain fault;
    fault.FailAt = 100;
    AllocationDomain domain(&fault, FaultDomain::Allocate, FaultDomain::Free);
    {
        Context context;
        Diagnostic error;
        REQUIRE(context.Initialize(Schema, domain, error) == Status::Ok);
        const auto calls = fault.Calls;
        fault.FailAt = calls;
        const Assignment edits[]{{ .Name = "render.width", .Data = Value::FromUint32(900) }};
        REQUIRE(context.Prepare(Layer::Session, edits, false, context.Revision(), error) == Status::Ok);
        REQUIRE(context.Commit(context.Revision(), error) == Status::Ok);
        CHECK(fault.Calls == calls);
    }
    CHECK(fault.Live == 0);
}
TEST_CASE("Config schema, bounds, encoding, and normalization")
{
    CHECK_FALSE(ValidName("render..width"));
    CHECK_FALSE(ValidName("Render.width"));
    CHECK(ValidName("render.msaa_2"));
    Descriptor invalid[]{{ .Name = "a", .Default = Value::FromBool(false), .Help = {}, .Units = {}, .Choices = {} },
                         { .Name = "a", .Default = Value::FromBool(true), .Help = {}, .Units = {}, .Choices = {} }};
    Context bad;
    Diagnostic error;
    CHECK(bad.Initialize(invalid, GetSystemAllocationDomain(), error) == Status::InvalidSchema);
    Value unchanged = Value::FromBool(true);
    CHECK(Value::FromText(Type::String, std::string_view("a\0b", 3), unchanged) == Status::InvalidValue);
    CHECK(unchanged.Kind == Type::Bool);
    Descriptor descriptor[]{
    {
        .Name = "x",
        .Default = Value::FromFloat32(0),
        .Help = {},
        .Units = {},
        .Choices = {},
        .MinReal = -2,
        .MaxReal = 2,
    }};
    Context context;
    REQUIRE(context.Initialize(descriptor, GetSystemAllocationDomain(), error) == Status::Ok);
    Value wide = Value::FromFloat64(0.1);
    wide.Kind = Type::Float32;
    Assignment edits[]{{ .Name = "x", .Data = wide }};
    REQUIRE(context.Prepare(Layer::Session, edits, false, context.Revision(), error) == Status::Ok);
    Binding binding;
    Value value;
    REQUIRE(context.Bind("x", binding) == Status::Ok);
    REQUIRE(context.ReadPrepared(binding, value) == Status::Ok);
    CHECK(value.Real == static_cast<float64>(static_cast<float32>(0.1)));
    context.Discard();
    edits[0].Data.Real = 3;
    CHECK(context.Prepare(Layer::Session, edits, false, context.Revision(), error) == Status::InvalidValue);
}

TEST_CASE("Config validates masked values, source masks, enums, edit quotas and text bounds")
{
    const std::string_view choices[]{"low", "high"};
    Value text;
    REQUIRE(Value::FromText(Type::Enum, "low", text) == Status::Ok);
    const Descriptor schema[]{
        {
            .Name = "quality.mode",
            .Default = text,
            .Help = {},
            .Units = {},
            .Choices = choices,
            .AllowedSources = SourceMask(Layer::Project) | SourceMask(Layer::Launch),
            .MaxTextBytes = 4,
        },
    };
    Context context;
    Diagnostic error;
    Limits limits;
    limits.MaxEdits = 1;
    REQUIRE(context.Initialize(schema, GetSystemAllocationDomain(), error, limits) == Status::Ok);
    Assignment high;
    high.Name = "quality.mode";
    REQUIRE(Value::FromText(Type::Enum, "high", high.Data) == Status::Ok);
    REQUIRE(context.Prepare(Layer::Launch, {&high, 1}, false, context.Revision(), error) == Status::Ok);
    REQUIRE(context.Commit(context.Revision(), error) == Status::Ok);
    Assignment masked;
    masked.Name = "quality.mode";
    REQUIRE(Value::FromText(Type::Enum, "bad", masked.Data) == Status::Ok);
    CHECK(context.Prepare(Layer::Project, {&masked, 1}, false, context.Revision(), error) == Status::InvalidValue);
    CHECK(context.Prepare(Layer::Session, {&high, 1}, false, context.Revision(), error) == Status::InvalidSource);
    const Assignment tooMany[]{high, high};
    CHECK(context.Prepare(Layer::Project, tooMany, false, context.Revision(), error) == Status::LimitExceeded);
    const auto explanation = Explain(context, "quality.mode");
    CHECK(explanation.Requested.Text.GetView() == "high");
}

TEST_CASE("Live constraints respect active restart options until actual restart")
{
    const Descriptor schema[]{
        { .Name = "render.width", .Default = Value::FromUint32(800), .Help = {}, .Units = {}, .Choices = {} },
        {
            .Name = "render.height",
            .Default = Value::FromUint32(5000),
            .Help = {},
            .Units = {},
            .Choices = {},
            .Group = 1,
            .Application = Apply::RestartEngine,
        },
    };
    Context context;
    Diagnostic error;
    REQUIRE(context.Initialize(schema, GetSystemAllocationDomain(), error, {}, PairConstraint) == Status::Ok);
    REQUIRE(context.SealStartup() == Status::Ok);
    const Assignment restart[]{{ .Name = "render.height", .Data = Value::FromUint32(1000) }};
    REQUIRE(context.Prepare(Layer::Session, restart, false, context.Revision(), error) == Status::Ok);
    REQUIRE(context.Commit(context.Revision(), error) == Status::PendingRestart);
    const Assignment live[]{{ .Name = "render.width", .Data = Value::FromUint32(1000) }};
    CHECK(context.Prepare(Layer::Session, live, false, context.Revision(), error) == Status::ConstraintFailed);
    CHECK(Explain(context, "render.width").Active.Unsigned == 800);
}
