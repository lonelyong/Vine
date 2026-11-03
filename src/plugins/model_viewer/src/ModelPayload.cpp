#include "ModelPayload.hpp"

namespace vn::model_viewer
{

// 载荷的元数据只在这里定义一次：Type::obtain 会返回已注册的那一个，所以重复定义不会再抛，但多一份代码没有必要。
VN_OBJECT_META_IMPL(MeshPayload, vn::Object)
VN_OBJECT_META_IMPL(MeshFilePayload, vn::Object)

} // namespace vn::model_viewer
