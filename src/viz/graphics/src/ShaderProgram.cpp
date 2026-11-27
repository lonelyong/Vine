#include <vine/graphics/ShaderProgram.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(ShaderProgram, vn::Object);

ShaderProgram::ShaderProgram()
  : id_(ContentRegistry::instance().allocate<ProgramTag>())
{}

ShaderProgram::~ShaderProgram()
{
    // The id leaves with the object: a plan or a facts row that still names it afterwards reads "gone" instead of
    // resolving to whatever the slot holds next (see ContentId.hpp).
    ContentRegistry::instance().release(id_.handle());
}

ShaderProgramDescription::ShaderProgramDescription(ProgramId id, std::uint64_t revision, String name, std::vector<ShaderStage> stages)
  : id_(id)
  , revision_(revision)
  , name_(std::move(name))
  , stages_(std::move(stages))
{}

ProgramId ShaderProgramDescription::id() const noexcept
{
    return id_;
}

std::uint64_t ShaderProgramDescription::revision() const noexcept
{
    return revision_;
}

const String& ShaderProgramDescription::name() const noexcept
{
    return name_;
}

std::span<const ShaderStage> ShaderProgramDescription::stages() const noexcept
{
    return stages_;
}

ShaderProgramDescriptionPtr ShaderProgram::description() const
{
    // The cache key IS the revision: a stage edit is the only thing that moves it, and a frame in flight keeps the
    // value it was published with. Several threads may describe one program at once and share one value (see
    // detail::DescriptionCache); a stage edit is still the owning thread's act (graphics-threading.md C22).
    return description_.get(
        [this] { return revision_; },
        [this](const std::uint64_t revision) { return new ShaderProgramDescription(id_, revision, name_, stages_); });
}

ProgramId ShaderProgram::id() const noexcept
{
    return id_;
}

String ShaderProgram::name() const
{
    return name_;
}

void ShaderProgram::setName(const String& name)
{
    name_ = name;
}

void ShaderProgram::addStage(const ShaderStage& stage)
{
    stages_.push_back(stage);
    ++revision_;
}

void ShaderProgram::clearStages()
{
    stages_.clear();
    ++revision_;
}

void ShaderProgram::replaceStages(const std::vector<ShaderStage>& stages)
{
    stages_ = stages;
    ++revision_;
}

bool ShaderProgram::setStage(std::size_t index, const ShaderStage& stage)
{
    if (index >= stages_.size()) {
        return false;
    }
    stages_[index] = stage;
    ++revision_;
    return true;
}

std::uint64_t ShaderProgram::revision() const
{
    return revision_;
}

std::size_t ShaderProgram::stageCount() const
{
    return stages_.size();
}

const ShaderStage* ShaderProgram::stage(std::size_t index) const
{
    return index < stages_.size() ? &stages_[index] : nullptr;
}

const std::vector<ShaderStage>& ShaderProgram::stages() const
{
    return stages_;
}

VN_GRAPHICS_NS_END
