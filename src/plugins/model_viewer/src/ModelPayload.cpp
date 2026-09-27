#include "ModelPayload.hpp"

namespace vn::model_viewer
{

// 载荷的元数据必须恰好一处定义：同一个 C++ 类型的第二个 Type 会让 Type 的构造函数抛 ITEM_ALREADY_EXISTS。
VN_OBJECT_META_IMPL(MeshPayload, vn::Object)
VN_OBJECT_META_IMPL(MeshFilePayload, vn::Object)

} // namespace vn::model_viewer
