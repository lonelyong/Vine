#include <vine/geometry/Model.hpp>

#include <utility>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(Model, vn::Object)

Model::Model()
{}

std::uint32_t Model::root() const noexcept
{
    return root_;
}

void Model::setRoot(std::uint32_t index) noexcept
{
    root_ = index;
}

const std::vector<Model::Node>& Model::nodes() const noexcept
{
    return nodes_;
}

void Model::setNodes(std::vector<Node> nodes)
{
    nodes_ = std::move(nodes);
}

const std::vector<Model::Entry>& Model::meshes() const noexcept
{
    return meshes_;
}

void Model::setMeshes(std::vector<Entry> meshes)
{
    meshes_ = std::move(meshes);
}

VN_GEOMETRY_NS_END
