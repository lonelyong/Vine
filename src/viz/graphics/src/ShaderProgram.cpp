#include <vine/graphics/ShaderProgram.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(ShaderProgram, vn::Object);

ShaderProgram::ShaderProgram() = default;

ShaderProgram::~ShaderProgram() = default;

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
