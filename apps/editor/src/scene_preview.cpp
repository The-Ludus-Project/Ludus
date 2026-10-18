#include "internal/scene_preview.h"
#include "editor_scene.h"

#include <ludus/graphics/rhi/rhi.h>

#include <type_traits>

namespace ludus::editor
{
namespace
{
namespace rhi = graphics::rhi;
using namespace foundation;
/// Record arrays declared by apps/world_demo/shaders/world_scene.slang (the world demo's MAX_RENDER_DRAWS).
/// The editor draws at most every collision box, every entity, one selection outline and the cursor.
constexpr usize SCENE_SHADER_RECORDS = 192;
constexpr usize SCENE_UNIFORM_VECTOR_LANES = 4;
constexpr usize SCENE_UNIFORM_VECTOR_BYTES = SCENE_UNIFORM_VECTOR_LANES * sizeof(float32);
constexpr usize SCENE_UNIFORM_GEOMETRY_OFFSET = 2 * SCENE_UNIFORM_VECTOR_BYTES;
constexpr usize SCENE_UNIFORM_COLOR_OFFSET =
    SCENE_UNIFORM_GEOMETRY_OFFSET + SCENE_SHADER_RECORDS * SCENE_UNIFORM_VECTOR_BYTES;
constexpr usize SCENE_UNIFORM_ROTATION_OFFSET =
    SCENE_UNIFORM_COLOR_OFFSET + SCENE_SHADER_RECORDS * SCENE_UNIFORM_VECTOR_BYTES;
constexpr usize SCENE_UNIFORM_BYTES = SCENE_UNIFORM_ROTATION_OFFSET + SCENE_SHADER_RECORDS * SCENE_UNIFORM_VECTOR_BYTES;
// Same layout as the world-demo shader, which checks the literal values: std140
// vector stride 16, offsets 0/16/32/3104/6176, total 9248 bytes. No GPU readback path.
struct alignas(SCENE_UNIFORM_VECTOR_BYTES) SceneUniforms final
{
    float32 View[SCENE_UNIFORM_VECTOR_LANES]{};
    float32 Settings[SCENE_UNIFORM_VECTOR_LANES]{};
    float32 Geometry[SCENE_SHADER_RECORDS][SCENE_UNIFORM_VECTOR_LANES]{};
    float32 Color[SCENE_SHADER_RECORDS][SCENE_UNIFORM_VECTOR_LANES]{};
    float32 Rotation[SCENE_SHADER_RECORDS][SCENE_UNIFORM_VECTOR_LANES]{};
};
static_assert(std::is_standard_layout_v<SceneUniforms> && sizeof(SceneUniforms) == SCENE_UNIFORM_BYTES);
static_assert(offsetof(SceneUniforms, Settings) == SCENE_UNIFORM_VECTOR_BYTES &&
              offsetof(SceneUniforms, Geometry) == SCENE_UNIFORM_GEOMETRY_OFFSET &&
              offsetof(SceneUniforms, Color) == SCENE_UNIFORM_COLOR_OFFSET &&
              offsetof(SceneUniforms, Rotation) == SCENE_UNIFORM_ROTATION_OFFSET);
bool Accepted(rhi::ResourceStatus status) noexcept
{
    return status == rhi::ResourceStatus::Ready || status == rhi::ResourceStatus::Pending;
}
struct SceneColor final
{
    float32 Red;
    float32 Green;
    float32 Blue;
};
void Box(SceneUniforms& u,
         usize& count,
         world_demo::Vec2 position,
         world_demo::Vec2 extent,
         float32 rotation,
         SceneColor color) noexcept
{
    u.Geometry[count][0] = position.X;
    u.Geometry[count][1] = position.Y;
    u.Geometry[count][2] = extent.X;
    u.Geometry[count][3] = extent.Y;
    u.Color[count][0] = color.Red;
    u.Color[count][1] = color.Green;
    u.Color[count][2] = color.Blue;
    u.Color[count][3] = 1;
    u.Rotation[count][0] = rotation;
    ++count;
}
} // namespace
ScenePreview::ScenePreview(SceneDocument* document, QObject* parent) : QObject(parent), Document_(document)
{
    Timer_.setInterval(16);
    connect(&Timer_, &QTimer::timeout, this, &ScenePreview::Tick);
}
ScenePreview::~ScenePreview()
{
    Stop();
}
platform::NativeWindowInfo ScenePreview::WindowInfo() const noexcept
{
    return Window_ ? Window_->GetNativeWindowInfo() : platform::NativeWindowInfo{};
}
bool ScenePreview::Start()
{
#if defined(Q_OS_WASM)
    Status_ = QStringLiteral("The separate authoring surface currently requires the desktop editor.");
    Q_EMIT Changed();
    return false;
#else
    Stop();
    if (Document_->Loaded() == false || rhi::GetStartup().State != rhi::StartupState::Idle || Surface_ == ~uint64{0})
    {
        return false;
    }
    platform::WindowManager manager;
    if (manager.Initialize({}) == false ||
        manager.CreateWindow(
            {
                .Name = "Ludus World authoring - WASD cursor, Enter pick, arrows transform, Esc cancel",
                .Width = 960,
                .Height = 640,
            },
            Window_) == false)
    {
        Status_ = QStringLiteral("Authoring window could not start; the World remains editable.");
        Q_EMIT Changed();
        return false;
    }
    Window_->AttachKeyboardSink({ .OnRecord = Record, .OnReset = Reset, .UserData = this });
    OwnsSession_ = true;
    ++Surface_;
    Width_ = Height_ = 0;
    Frames_ = 0;
    Cursor_ = Document_->Draft().Level.Camera;
    const auto status = rhi::Start({ .Name = "Ludus World authoring", .Version = 1 }, Window_->GetNativeWindowInfo());
    if (status != rhi::StartStatus::Ready && status != rhi::StartStatus::Pending)
    {
        Stop();
        Status_ = QStringLiteral("Rendering could not start; the World remains editable.");
        Q_EMIT Changed();
        return false;
    }
    Status_ = QStringLiteral("Opening authoring surface…");
    Timer_.start();
    Q_EMIT Changed();
    return true;
#endif
}
void ScenePreview::Stop() noexcept
{
    Timer_.stop();
    Document_->CancelTransform();
    if (Window_)
    {
        Window_->DetachKeyboardSink();
    }
    if (OwnsSession_)
    {
        rhi::Shutdown();
    }
    OwnsSession_ = false;
    Window_.Reset();
    Created_ = Ready_ = false;
    Vertex_ = {};
    Fragment_ = {};
    Uniform_ = {};
    Pipeline_ = {};
    Presented_ = {};
    QueueCount_ = 0;
    for (auto& arrow : Arrows_)
    {
        arrow = false;
    }
    for (auto& suppressed : Suppressed_)
    {
        suppressed = false;
    }
    Cancel_ = false;
}
void ScenePreview::Select(const world_demo::Name& id) noexcept
{
    Document_->CancelTransform();
    Selected_ = id;
}
void ScenePreview::Record(void* context, const input::KeyboardRecord& record) noexcept
{
    auto& self = *static_cast<ScenePreview*>(context);
    if (self.QueueCount_ == 64)
    {
        self.QueueCount_ = 0;
        self.Cancel_ = true;
        return;
    }
    self.Queue_[self.QueueCount_++] = record;
}
void ScenePreview::Reset(void* context, input::ResetReason, const input::FocusBaseline& baseline) noexcept
{
    auto& self = *static_cast<ScenePreview*>(context);
    self.QueueCount_ = 0;
    self.Cancel_ = true;
    for (usize i = 0; i < input::KEY_COUNT; ++i)
    {
        self.Suppressed_[i] = baseline.Keys[i];
    }
}
void ScenePreview::Input()
{
    using input::Key;
    bool changed = Cancel_ && Document_->Previewing();
    if (Cancel_)
    {
        Document_->CancelTransform();
        for (auto& arrow : Arrows_)
        {
            arrow = false;
        }
        Cancel_ = false;
    }
    for (usize i = 0; i < QueueCount_; ++i)
    {
        const auto& record = Queue_[i];
        const auto key = record.PhysicalKey;
        if (input::IsValidKey(key) == false)
        {
            continue;
        }
        const auto index = input::KeyIndex(key);
        const bool down = record.Transition == input::KeyTransition::Down;
        if (down == false)
        {
            Suppressed_[index] = false;
        }
        if (Suppressed_[index] || record.Repeat)
        {
            continue;
        }
        const int32 arrow = key == Key::ArrowLeft    ? 0
                            : key == Key::ArrowRight ? 1
                            : key == Key::ArrowUp    ? 2
                            : key == Key::ArrowDown  ? 3
                                                     : -1;
        if (arrow >= 0)
        {
            Arrows_[arrow] = down;
            if (down)
            {
                const auto selected = SceneDocument::Find(Document_->Visible(), Selected_);
                if (selected == Document_->Visible().Level.EntityCount)
                {
                    continue;
                }
                if (Document_->Previewing() == false &&
                    Document_->BeginTransform(&Selected_, 1, Document_->Stamp(0, Surface_)) == false)
                {
                    continue;
                }
                const auto& e = Document_->Visible().Level.Entities[selected];
                SceneTransform value{e.Position, e.Rotation, e.Scale};
                value.Position.X += arrow == 0 ? -0.1F : arrow == 1 ? 0.1F : 0;
                value.Position.Y += arrow == 2 ? 0.1F : arrow == 3 ? -0.1F : 0;
                (void)Document_->PreviewTransform(&value, 1);
                changed = true;
            }
            else if (Arrows_[0] == false && Arrows_[1] == false && Arrows_[2] == false && Arrows_[3] == false)
            {
                (void)Document_->AcceptTransform();
                changed = true;
            }
        }
        else if (down && key == Key::Escape)
        {
            Document_->CancelTransform();
            changed = true;
        }
        else if (down && key == Key::Enter)
        {
            world_demo::Name selected;
            if (Document_->Pick(Cursor_, Presented_, Document_->Stamp(0, Surface_), selected))
            {
                Select(selected);
                changed = true;
            }
        }
        else if (down && key == Key::Tab && Document_->Previewing() == false)
        {
            const auto& scene = Document_->Draft();
            const auto next = (SceneDocument::Find(scene, Selected_) + 1) % scene.Level.EntityCount;
            Select(scene.Level.Entities[next].Id);
            changed = true;
        }
        else if (down)
        {
            Cursor_.X += key == Key::KeyA ? -0.25F : key == Key::KeyD ? 0.25F : 0;
            Cursor_.Y += key == Key::KeyW ? 0.25F : key == Key::KeyS ? -0.25F : 0;
        }
    }
    QueueCount_ = 0;
    if (changed)
    {
        Q_EMIT Changed();
    }
}
bool ScenePreview::Prepare() noexcept
{
    if (Created_ == false)
    {
        if (Accepted(rhi::CreateShader(shaders::editor_scene::Vertex(), Vertex_)) == false ||
            Accepted(rhi::CreateShader(shaders::editor_scene::Fragment(), Fragment_)) == false ||
            Accepted(rhi::CreateUniform(sizeof(SceneUniforms), Uniform_)) == false)
        {
            return false;
        }
        Created_ = true;
    }
    const rhi::ResourceStatus resources[] = {rhi::GetStatus(Vertex_),
                                             rhi::GetStatus(Fragment_),
                                             rhi::GetStatus(Uniform_)};
    for (auto status : resources)
    {
        if (status == rhi::ResourceStatus::Pending)
        {
            return true;
        }
        if (status != rhi::ResourceStatus::Ready)
        {
            return false;
        }
    }
    if (rhi::GetStatus(Pipeline_) == rhi::ResourceStatus::InvalidHandle &&
        Accepted(rhi::CreatePipeline({Vertex_, Fragment_, Uniform_}, Pipeline_)) == false)
    {
        return false;
    }
    const auto status = rhi::GetStatus(Pipeline_);
    Ready_ = status == rhi::ResourceStatus::Ready;
    return Ready_ || status == rhi::ResourceStatus::Pending;
}
bool ScenePreview::Render() noexcept
{
    auto status = rhi::SetFrameTarget({ .Width = Width_, .Height = Height_ });
    if (status == rhi::FrameStatus::Skipped)
    {
        return true;
    }
    if (status != rhi::FrameStatus::Ready)
    {
        return false;
    }
    status = rhi::BeginFrameStatus();
    if (status == rhi::FrameStatus::Skipped)
    {
        return true;
    }
    if (status != rhi::FrameStatus::Ready)
    {
        return false;
    }
    const auto frame = rhi::GetFrameInfo();
    SceneUniforms u;
    u.View[0] = static_cast<float32>(frame.Width);
    u.View[1] = static_cast<float32>(frame.Height);
    const auto& level = Document_->Visible().Level;
    u.View[2] = level.Camera.X;
    u.View[3] = level.Camera.Y;
    u.Settings[0] = level.VerticalExtent;
    usize count = 0;
    for (usize i = 0; i < level.CollisionCount; ++i)
    {
        Box(u, count, level.Collision[i].Center, level.Collision[i].HalfExtent, 0, {0.25F, 0.28F, 0.32F});
    }
    for (usize i = 0; i < level.EntityCount; ++i)
    {
        const auto& e = level.Entities[i];
        const world_demo::Vec2 extent{e.HalfExtent.X * e.Scale.X, e.HalfExtent.Y * e.Scale.Y};
        if (e.Id == Selected_)
        {
            Box(u,
                count,
                e.Position,
                {extent.X + 0.06F, extent.Y + 0.06F},
                e.Rotation,
                {1, Document_->Previewing() ? 0.45F : 0.85F, 0.1F});
        }
        Box(u,
            count,
            e.Position,
            extent,
            e.Rotation,
            {e.Type == world_demo::Kind::Enemy ? 0.8F : 0.15F,
             e.Type == world_demo::Kind::Exit ? 0.8F : 0.3F,
             e.Type == world_demo::Kind::Player ? 0.9F : 0.2F});
    }
    Box(u, count, Cursor_, {0.15F, 0.01F}, 0, {1, 1, 1});
    Box(u, count, Cursor_, {0.01F, 0.15F}, 0, {1, 1, 1});
    u.Settings[1] = static_cast<float32>(count);
    // Existing RHI byte-view boundary; no standard container is owned here.
    const auto upload = rhi::UpdateUniform(Uniform_, {reinterpret_cast<const uint8*>(&u), sizeof(u)});
    const auto draw = upload == rhi::ResourceStatus::Ready ? rhi::DrawFullscreen(Pipeline_) : upload;
    const auto end = rhi::EndFrameStatus();
    if (draw != rhi::ResourceStatus::Ready)
    {
        return false;
    }
    if (end == rhi::FrameStatus::Ready)
    {
        ++Frames_;
        Presented_ = Document_->Stamp(0, Surface_);
    }
    return end == rhi::FrameStatus::Ready || end == rhi::FrameStatus::Skipped;
}
void ScenePreview::Tick()
{
    if (Window_.Get() == nullptr)
    {
        return;
    }
    if (Document_->Loaded() == false)
    {
        Stop();
        return;
    }
    if (Window_->HandleEvent({}) == false)
    {
        Stop();
        Status_ = QStringLiteral("Authoring surface closed.");
        Q_EMIT Changed();
        return;
    }
    const auto window = Window_->GetNativeWindowInfo();
    if (window.Width != Width_ || window.Height != Height_)
    {
        if (Surface_ == ~uint64{0})
        {
            Stop();
            return;
        }
        ++Surface_;
        Width_ = window.Width;
        Height_ = window.Height;
        Document_->CancelTransform();
    }
    Input();
    const auto state = rhi::GetStartup().State;
    if (state == rhi::StartupState::Failed || state == rhi::StartupState::DeviceLost ||
        (state == rhi::StartupState::Ready && (Prepare() == false || (Ready_ && Render() == false))))
    {
        Stop();
        Status_ = QStringLiteral("Authoring rendering stopped; the World remains editable and saveable.");
        Q_EMIT Changed();
        return;
    }
    if (Frames_ == 1)
    {
        Status_ = QStringLiteral(
            "Scene rendered. WASD moves the pick cursor; Enter selects; Tab cycles; arrows transform; Escape cancels.");
        Q_EMIT Changed();
    }
}
} // namespace ludus::editor
