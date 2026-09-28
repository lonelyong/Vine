# Robotics IO 模块设计（XML 序列化）

> 状态：设计稿（先设计，未实现）→ **阶段一（VFS 基础设施）已实现**：`vn::io` 的 `Vfs` /
> `ZipVfs` / `DirectoryVfs` + `ZipArchive` 后端重载已落地，测试 `tests/test_iobase/VfsTest.cpp`
> （14 用例）全通过。**阶段二（场景序列化）已实现**：`RoboticsIO` 的 `XmlIOBase` / `DeviceIO` /
> `WorkcellIO` 已落地。**阶段三（打包）已实现**：`loadPkg` / `savePkg`（`.vdevpkg` / `.vwspkg`，
> 纯内存 zip 流无临时目录）、mesh 原生 bin + XML 描述（`geoms/*.bin`）、嵌套设备包与共享 mesh
> 去重；`tests/test_robotics_io/` 全通过（15 用例，含 `PkgIOTest` 8 用例）。**最佳实践重构已
> 完成**：`XmlIOBase` / `DeviceIO` / `WorkcellIO` 全部无状态（每次操作的状态都在 Parse/Export
> context 中，含 workcell 指针、VFS、目录、警告缓冲），实例可重入、可安全复用；移除了
> `const_cast` 与 `friend class WorkcellIO`（`DeviceIO::loadXmlFromVfs` 改为公开的“内部”方法）。
>
> **最终公共 API = 5 版本**（`DeviceIO` 与 `WorkcellIO` 一致，已移除全部 `saveXml`）：
> 1. `loadXml(file_path)` —— 从未打包文件夹加载（资源在 xml 同目录）；
> 2. `loadPkg(pkg_path)` —— 从 pkg 文件加载；
> 3. `loadPkg(vfs)` —— 从已打开 / 内存构造的 VFS 加载；
> 4. `savePkg(obj, pkg_path)` —— 直接打包到指定路径文件；
> 5. `savePkg(obj, vfs)` —— 所有资源输出到 VFS（含 xml）。
>
> XML 入口固定在 VFS 根目录：设备 `device.xml`、工作站 `workcell.xml`。
> 配套工具：`tools/urdf2vine`（URDF + 二进制 STL → `.vdevpkg`，复用 `DeviceIO::savePkg`）。
>
> **API 现状**：`Vfs`（原 `IMemoryVfs`）已按 `.ai/design/iobase-vfs-design.md` §13 做过“一次断代”重设计 ——
> 所有读写返回 `IoError` / `Result<T>`，旧 `bool` 家族（`writeFile` / `readFile` / `mountFile` /
> `save(vector&)`）**已删除**。因此**第 6 / 8 节的接口清单是历史设计稿**，实际签名以
> `Vfs.hpp`（原 `IMemoryVfs.hpp`）与 `sdk/vine/io/ZipVfs.hpp` 为准。 <!-- drift-ok -->
> **命名提醒**：本文（尤其第 6 节与第 2/3 节的类型草图）出现的 `IMemoryVfs` / `ZipMemoryVfs` 是**历史名**，
> 现名分别为 `Vfs` / `ZipVfs`；`save` / `serialize` / `openZip` 同理是历史拼写，现名 `saveAs` / `toBytes` / `open(…, OpenMode::ReadOnly)`。
> **ZIP 职责分层（S3b）**：`ZipVfs::open(…, OpenMode::ReadOnly)` 是打开已有包的**唯一入口**（惰性只读），
> `ZipVfs` 建树后由 `saveAs` / `toBytes` 出包、`commit` 写回；`ZipArchive` 是存储层
> （`open` / `index` / `read` / `openRead` / `saveAs` / `commit`）。`ZipMemoryVfs`、
> `ZipVfs::openZip`、`IMemoryVfs` 与 `ZipArchive::entryNames` 均已删除。
> 参考：VMR `D:\PROJS\VMR\src\scene_core\include\vmr\io`（VDeviceIO / VWorkcellIO / VUrdfIOBase）
> 约束：不用 URDF 格式，仍用 XML；遵循 Vine 命名（无 `V` 前缀、无 `getXXX`）；C++20。

## 1. 背景与目标

为 `vn::robotics::workcell` 的 `Device` / `MotionDevice` / `Scanner` / `Workcell` 提供 XML
序列化与反序列化，新建独立模块 `RoboticsIO`。放弃 URDF 的 `<robot>` 根节点语义，采用自研的
`<device>` / `<workcell>` 根节点格式。

**实施顺序（基础设施优先）**：
1. **阶段一（已完成）**：VFS 基础设施 —— `vn::io`（IOBase）新增 `Vfs` / `ZipVfs` /
   `DirectoryVfs`，并给 `vn::io::ZipArchive` 补后端重载（见第 6 节）；测试 `VfsTest`。
2. **阶段二（已完成）**：场景序列化 —— `RoboticsIO` 的 `XmlIOBase` / `DeviceIO` / `WorkcellIO`
   （`loadXml` / `saveXml`）基于 VFS 落地；配套模型改动（见第 7 节）。
3. **阶段三（已完成）**：打包 —— `loadPkg` / `savePkg`（zip 流，无临时目录），mesh 原生 bin，BRep（见第 9 节）。

## 2. 模块结构（新建）

```
src/robotics/
  CMakeLists.txt                          # 增加 add_subdirectory(io)
  io/
    CMakeLists.txt                        # vn_add_library(ROBOTICSIO_TARGET_NAME RoboticsIO)
    sdk/vine/robotics/io/
      robot_io_global.hpp                 # VN_ROBOTICS_IO_API + VN_ROBOTICS_IO_NS_BEGIN/END
      XmlIOBase.hpp                       # 公共基类（原 VUrdfIOBase，去掉 URDF 语义）
      DeviceIO.hpp                        # 设备序列化
      WorkcellIO.hpp                      # 工作站序列化
    src/
      XmlIOBase.cpp
      DeviceIO.cpp
      WorkcellIO.cpp
      IoUtils.hpp / IoUtils.cpp           # 内部工具：Q<->字符串、姿态<->XML、材质/形状注册表
```

`src/robotics/CMakeLists.txt` 由当前只包含 `add_subdirectory(core)` 增加 `add_subdirectory(io)`。

`CMakeLists.txt`（沿用 meshio/iobase 的 FetchContent 静态烤进 DLL 模式）：

```cmake
vn_add_library(ROBOTICSIO_TARGET_NAME RoboticsIO)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
if(VINE_USE_FETCHCONTENT)
    FetchContent_Declare(tinyxml2
        GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
        GIT_TAG        10.0.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(tinyxml2)
else()
    find_package(tinyxml2 CONFIG REQUIRED)
endif()

target_link_libraries(${ROBOTICSIO_TARGET_NAME} PUBLIC vn::RoboticsCore vn::Geometry vn::IOBase)
target_link_libraries(${ROBOTICSIO_TARGET_NAME} PRIVATE tinyxml2)
```

说明：
- XML 库选 tinyxml2（与 VMR 一致、轻量、无依赖），静态烤进 `viRoboticsIO*.dll`。
- `RoboticsIO` 依赖 `RoboticsCore`（workcell/kinematics）、`Geometry`（Shape/Material）与
  `IOBase`（`vn::io` 的 `Vfs`/`ZipVfs`/`ZipArchive`，见第 6 节），
  不反向依赖，无环。
- `robot_io_global.hpp` 中 `VN_ROBOTICS_IO_API` 由 `vn_add_library` 生成的 `VN_ROBOTICSIO_LIB`
  编译宏决定 `VN_EXPORT`/`VN_IMPORT`。

## 3. 类设计

### 3.1 `XmlIOBase`（公共基类，对应 VMR `VUrdfIOBase`）

```cpp
namespace vn::robotics::io {

class VN_ROBOTICS_IO_API XmlIOBase {
  protected:
    struct ExportOptions {
        double len_unit_scaling = 1.0;  // 导出长度单位缩放
    };
    struct ExportContext {
        ExportOptions& options;
        const workcell::Workcell* cell{ nullptr };  // 只读帧查找（worldFrame/findSceneObjectByFrame）
        vn::io::Vfs*     vfs{ nullptr };   // 打包资源（geoms bins）；裸 XML 为 null
        String                    vfs_dir;          // 文档所在 VFS 目录（"" = 根）
        std::string               msgs;             // 本次导出的非致命警告
        std::size_t               geom_seq{ 0 };    // geoms bin 命名序号
        std::map<const vn::geometry::Shape*, String> mesh_paths;  // 共享 mesh 去重
        explicit ExportContext(ExportOptions& opts) : options(opts) {}
    };

    struct ParseOptions {
        double len_unit_scaling = 1.0;  // 解析长度单位缩放
    };
    struct ParseContext {
        ParseOptions& options;
        workcell::Workcell* cell{ nullptr };  // 正在填充的工作站（addSceneObject）；非工作站解析为 null
        vn::io::Vfs* vfs{ nullptr };
        String vfs_dir;
        std::string msgs;
        std::map<String, vn::intrusive_ptr<vn::geometry::Material>> materials_by_name;  // 材质库
        explicit ParseContext(ParseOptions& opts) : options(opts) {}
    };

  public:
    virtual ~XmlIOBase() = default;  // 无成员，实例无状态、可重入

  protected:
    // 版本（<device version="1.0"> / <workcell version="1.0">）
    void parseVersion(ParseContext&, uint16_t& major, uint16_t& minor, const tinyxml2::XMLElement*);
    void exportVersion(ExportContext&, uint16_t major, uint16_t minor, tinyxml2::XMLElement*);
    // 姿态（四元数，无损往返）
    void parsePose(ParseContext&, math::Isometry3d& pose, const tinyxml2::XMLElement* xe_origin);
    void exportPose(ExportContext&, const math::Isometry3d& pose, tinyxml2::XMLElement*);
    // 材质 / 几何（visual/collision 共用）
    vn::intrusive_ptr<vn::geometry::Material> parseMaterial(ParseContext&, const tinyxml2::XMLElement*);
    vn::intrusive_ptr<vn::geometry::Shape> parseGeometry(ParseContext&, const tinyxml2::XMLElement*);
    void exportGeometry(ExportContext&, const vn::geometry::Shape&, tinyxml2::XMLElement*);
    void parseVisual(ParseContext&, workcell::Visual&, const tinyxml2::XMLElement*);
    void exportVisual(ExportContext&, const workcell::Visual&, tinyxml2::XMLElement*);
    void parseCollision(ParseContext&, workcell::Collision&, const tinyxml2::XMLElement*);
    void exportCollision(ExportContext&, const workcell::Collision&, tinyxml2::XMLElement*);
    // 连杆 / 关节（设备与工作站共用）
    void parseLink(ParseContext&, workcell::Link&, const tinyxml2::XMLElement*);
    void exportLink(ExportContext&, const workcell::Link&, tinyxml2::XMLElement*);
    std::unique_ptr<workcell::Joint> parseJoint(ParseContext&, tinyxml2::XMLElement*);
    void exportJoint(ExportContext&, const workcell::Joint&, tinyxml2::XMLElement*);
    // 设备元数据
    void parseDeviceMetadata(ParseContext&, workcell::DeviceMetadata&, const tinyxml2::XMLElement*);
    void exportDeviceMetadata(ExportContext&, const workcell::DeviceMetadata&, tinyxml2::XMLElement*);
};

} // namespace vn::robotics::io
```

要点：
- 上下文携带 `len_unit_scaling`（单位缩放）、活动 `vfs`、`vfs_dir`、工作站指针与警告缓冲
  `msgs`；`warning()` / `msgs()` / `clearMsgs()` 类级成员已移除，改为内部
  `detail::appendWarning(ctx.msgs, fmt, ...)`（`src/IoUtils.hpp`）。
- 无类级可变状态 → `XmlIOBase` / `DeviceIO` / `WorkcellIO` 实例可重入、可安全复用（含多线程，
  每次操作自带 context）；加载失败用 `std::runtime_error` 抛异常。
- `exportMaterial` 已移除（禁止就地材质）；`parseMaterial` 仅用于读取设备材质库条目。
- 连杆/关节/材质/几何解析复用（设备文件与工作站内联几何都用同一套）。

### 3.2 `DeviceIO`（对应 VMR `VDeviceIO`）

```cpp
class VN_ROBOTICS_IO_API DeviceIO : public XmlIOBase {
  public:
    struct LoadOptions {
        std::optional<workcell::LengthUnit> source_len_unit; // 文件内单位（默认按 metadata）
        std::optional<workcell::LengthUnit> target_len_unit; // 读入内存的单位（默认 mm）
    };
    struct SaveOptions {
        std::optional<workcell::LengthUnit> source_len_unit; // 内存单位（默认 mm）
        std::optional<workcell::LengthUnit> target_len_unit; // 写入文件的单位（默认 m）
    };

  public:
    /// 从文件加载设备；失败抛 std::runtime_error。
    std::unique_ptr<workcell::Device> loadXml(const std::filesystem::path& file_path,
                                              const LoadOptions& options = {});
    /// 从 XML 字符串加载设备。
    std::unique_ptr<workcell::Device> loadXml(const String& xml_str,
                                              const LoadOptions& options = {});
    /// 导出设备为 XML 文档（所有权转移给调用方）。
    std::unique_ptr<tinyxml2::XMLDocument> saveXml(const workcell::Device& dev,
                                                   const SaveOptions& options = {});
    /// 导出设备为 XML 并写入文件。
    void saveXml(const workcell::Device& dev, const std::filesystem::path& save_to,
                 const SaveOptions& options = {});

    /// 从设备包加载（zip，直接读条目，不解压；见第 6 节）——**阶段三**。
    std::unique_ptr<workcell::Device> loadPkg(const std::filesystem::path& pkg_path,
                                              const LoadOptions& options = {});
    /// 导出设备为设备包（内存文档一次成型，无临时目录；见第 6 节）——**阶段三**。
    void savePkg(const workcell::Device& dev, const std::filesystem::path& pkg_path,
                 const SaveOptions& options = {});

  private:
    std::unique_ptr<workcell::Device> parseDeviceInternal(ParseContext&, tinyxml2::XMLElement* xe_device,
                                                          const LoadOptions&);
    std::unique_ptr<workcell::DeviceData> parseDeviceData(ParseContext&, tinyxml2::XMLElement* xe_device,
                                                          const LoadOptions&);
    void parseScannerCameras(ParseContext&, workcell::ScannerData&, tinyxml2::XMLElement*);
    void parseScannerProjectors(ParseContext&, workcell::ScannerData&, tinyxml2::XMLElement*);
    void exportDeviceInternal(ExportContext&, const workcell::Device&, tinyxml2::XMLElement* xe_device,
                              const SaveOptions&);
    void exportScannerCameras(ExportContext&, const workcell::Scanner&, tinyxml2::XMLElement*);
    void exportScannerProjectors(ExportContext&, const workcell::Scanner&, tinyxml2::XMLElement*);
};
```

关键设计：
- **返回类型**：`std::unique_ptr<workcell::Device>`（Vine 的 Device 由 Workcell 以 unique_ptr 独占，
  不引入 shared_ptr 语义）。
- **加载工厂**：按 `<device kind>` 创建具体类型——`Scanner` → `Scanner`，其余
  （Manipulator / ExternalAxis / Positioner / Tool / Other）→ `MotionDevice`
  （当前 Vine 没有 Manipulator/ExternalAxis/Positioner 子类，统一 `MotionDevice` + `SerialKinematics`，
  与"现在都当串联链"的约定一致）。
- **加载流程**：解析 `<metadata>`（取得单位/逆解类型）→ 解析 `<material>`/`<link>`/`<joint>`
  入 `ParseContext`（材质查重、连杆名查重）→ 组装 `DeviceData`（MotionDeviceData / ScannerData）
  → `dev->init(std::move(data))` → 若为 MotionDevice 且 metadata 有 iksolver，
  `dev->kinematics()->setIKSolverType(...)` → 若为 Scanner，`parseScannerCameras/Projectors`
  在 init 前把数据填进 ScannerData（frame 绑定由 initScanner 按 frame 名完成）。
- **单位**：先解析 metadata 拿 `length_unit`，再据 LoadOptions 计算 `len_unit_scaling`，
  用于长度字段（原点平移、相机 near/far、投影仪景深）缩放回内存单位（mm）。

### 3.3 `WorkcellIO`（对应 VMR `VWorkcellIO`）

```cpp
class VN_ROBOTICS_IO_API WorkcellIO : public XmlIOBase {
  public:
    struct SaveOptions {
        bool ignore_part{ false }; // 预留：不保存零件
    };

  public:
    std::unique_ptr<workcell::Workcell> loadXml(const std::filesystem::path& file_path);
    void saveXml(const workcell::Workcell& cell, const std::filesystem::path& save_to,
                 const SaveOptions& options = {});

    /// 从工作站包加载（zip，直接读条目，不解压；见第 6 节）——**阶段三**。
    std::unique_ptr<workcell::Workcell> loadPkg(const std::filesystem::path& pkg_path);
    /// 导出工作站为工作站包（内存文档一次成型，无临时目录；见第 6 节）——**阶段三**。
    void savePkg(const workcell::Workcell& cell, const std::filesystem::path& pkg_path,
                 const SaveOptions& options = {});

  private:
    // 导出
    void exportObject(ExportContext&, const workcell::SceneObject&, tinyxml2::XMLElement* xe_parent);
    void exportObjectCommon(ExportContext&, const workcell::SceneObject&, tinyxml2::XMLElement* xe);
    void exportRigidObject(ExportContext&, const workcell::RigidObject&, tinyxml2::XMLElement* xe);
    void exportDevice(ExportContext&, const workcell::Device&, tinyxml2::XMLElement* xe);
    // 解析
    void parseObject(ParseContext&, raw_ptr<workcell::SceneObject> parent, tinyxml2::XMLElement* xe);
    void parseObjectCommon(ParseContext&, workcell::SceneObject&, tinyxml2::XMLElement* xe);
    std::unique_ptr<workcell::SceneObject> parseRigidObject(ParseContext&, tinyxml2::XMLElement* xe);
    std::unique_ptr<workcell::Device> parseDevice(ParseContext&, tinyxml2::XMLElement* xe);
};
```

关键设计：
- **设备引用**：工作站中设备以 `<obj type="device" file="devices/xxx.vdev"/>` 引用独立设备文件
  （VMR 风格），通过 `DeviceIO` 加载；避免重复、支持设备复用。
- **对象树**：递归导出/解析 `<children>`，父先于子；`<obj>` 通用属性
  `name` / `type` / `parent_frame` + `<origin>`（= 对象 base 帧相对父帧的固定变换）。
- **挂载帧**：`parent_frame` 指定父对象的某个帧（缺省 = 父对象 base 帧）。解析时用
  `workcell->addSceneObject(obj, parent_frame)`，帧按名在父对象 `frames()` 中查找
  （设备需先 init 完成，帧才可用——递归序保证父先建）。
- **运动设备实例属性**：`home` / `q` / `resolutions`（空格分隔数值），
  在设备 init 后设置：`setHomeQ`、`setQ(q, state)`、`kinematics()->setJointResolutions(...)`。
- **刚体对象**：内联 `<visual>` / `<collision>`。

## 4. XML 格式设计

### 4.1 设备文件 `.vdev`

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!-- This file is automatically generated by Vine on 2026-09-01 at 12:00:00. -->
<device name="robot" kind="Manipulator" version="1.0">
  <metadata id=".." name="robot" description=".." sn=".." manufacturer=".." model=".."
            author=".." version="1.0" create_time=".." modified_time=".."
            length_unit="mm" iksolver="Jacobian"/>

  <!-- 全局材质表（按名引用） -->
  <material name="aluminium">
    <color rgba="0.8 0.8 0.8 1.0"/>   <!-- 或 <phong>..</phong> / <pbr>..</pbr> -->
  </material>

  <link name="base">
    <visual>
      <origin xyz="0 0 0" quat="0 0 0 1"/>
      <geometry><box size="0.2 0.2 0.2"/></geometry>
      <material name="aluminium"/>
    </visual>
    <collision>
      <origin xyz="0 0 0" quat="0 0 0 1"/>
      <geometry><sphere radius="0.1"/></geometry>
    </collision>
  </link>

  <link name="link1">
    <visual>
      <origin xyz="0 0 0" quat="0 0 0 1"/>
      <!-- mesh：引用包内二进制条目（顶点/法线/索引，见 6.8）；纯 XML 时为相对文件路径 -->
      <geometry>
        <indexed_triangle_mesh vertex_count="12" triangle_count="4"
                               positions="geoms/link1.positions.bin"
                               normals="geoms/link1.normals.bin"
                               indices="geoms/link1.indices.bin"/>
      </geometry>
    </visual>
  </link>

  <joint name="j1" type="revolute" parent="base" child="link1">
    <origin xyz="0 0 0.1" quat="0 0 0 1"/>   <!-- 关节固定变换 Frame::fixedTransform -->
    <dof type="revolute" axis="0 0 1">       <!-- 每个自由度一个 -->
      <origin xyz="0 0 0" quat="0 0 0 1"/>   <!-- DofInfo.origin -->
      <limit lower="-3.14" upper="3.14" velocity="1.5" acceleration="2.0"/>
    </dof>
  </joint>

  <!-- 仅扫描仪 -->
  <camera frame="left_cam">
    <design   width="1920" height="1080" center_x="960" center_y="540"
              focus_x="1200" focus_y="1200" near="1" far="1000"/>
    <calibrated .../>
  </camera>
  <projector frame="proj">
    <design fov_w="1.0" fov_h="0.8" min_dof="300" max_dof="1200" measure_dist="800" crop_value="0.8"/>
    <calibrated .../>
  </projector>
  <parameters><param key="aa.bb" type="double" value="1.0"/></parameters>
</device>
```

与 URDF 的差异：
- 根节点 `<device>`（替代 `<robot>`），`kind` 属性替代 `x-kind`。
- 关节 `type` 保留 `fixed / revolute / prismatic / planar`（对应 `kinematics::FrameType`）。
- 每个 DoF 用 `<dof>` 显式描述（planar 关节 = 3 个 `<dof>`）。
- 姿态统一四元数 `quat="x y z w"`（`Isometry3d` 原生四元数，无损往返；不做 RPY 转换）。
- 长度单位写入 metadata `length_unit`，加载时据此缩放。
- `<geometry>` 形状：box / sphere / cylinder / cone / ellipsoid / triangle_mesh /
  indexed_triangle_mesh；`<brep>`（BRep/STEP）本期不实现，见 6.9。

### 4.2 工作站文件 `.vcell`

```xml
<?xml version="1.0" encoding="UTF-8"?>
<workcell name="demo" version="1.0">
  <metadata name="demo" description=".."/>

  <obj name="robot1" type="device" file="devices/robot.vdev" kind="Manipulator"
       home="0 0 0" q="0.1 -0.2" resolutions="0.05 0.05">
    <origin xyz="0 0 0" quat="0 0 0 1"/>
    <children>
      <obj name="cam" type="device" file="devices/cam.vdev" kind="Scanner"
           parent_frame="robot1_tcp">   <!-- 挂到机器人末端帧 -->
        <origin xyz="0.1 0 0.2" quat="0 0 0 1"/>
      </obj>
    </children>
  </obj>

  <obj name="table" type="rigid_object">
    <origin xyz="1 0 0" quat="0 0 0 1"/>
    <visual><origin .../><geometry><box size="1 0.1 0.8"/></geometry></visual>
    <collision><origin .../><geometry><box size="1 0.1 0.8"/></geometry></collision>
  </obj>
</workcell>
```

> 设备引用（`file`）可以是：
> - `.vdev`：纯 XML 设备文件，直接解析；
> - `.vdevpkg`：**设备包**（自身是一个 zip），作为条目嵌套在场景包内；加载时按扩展名分发，
>   从场景包读出该条目字节，再打开成嵌套的 `ZipMemoryVfs` 解析（见 6.6"包中包"）。
>
> **设备实例复用**：多个 `<obj>` 可指向同一 `file`（同一份设备定义），实例差异只在 `<obj>`
> 属性（name / origin / parent_frame / home / q / resolutions）；导出时定义只写一次（见 6.10）。

## 5. 数据映射表

| Vine 模型 | XML | 说明 |
|---|---|---|
| `DeviceMetadata` | `<metadata>` | id/name/description/sn/manufacturer/model/author/version + **新增** create_time / modified_time / length_unit / iksolver |
| `DeviceData.materials` | `<materials>` | 命名材质库 `<material name color/>`，visual 按名引用 |
| `DeviceData.links` | `<link>` | name + `RigidBody`（visual + collision） |
| `RigidBody.visuals` | `<visual>` | shape + origin + material（引用全局材质名，无 name 则内联） |
| `RigidBody.collisions` | `<collision>` | shape + origin |
| `Link` base 帧 | 隐式 | 由关节 parent/child 图构建；base link 挂设备 base 帧 |
| `Joint` | `<joint>` | type（FrameType）+ origin（fixedTransform）+ 多 `<dof>` + parent/child link 名 |
| `DofInfo` | `<dof>` | type + axis + origin(xyz/quat) + lower/upper/velocity/acceleration 全为属性
| `DeviceKind` | `<device kind>` | 加载工厂：Scanner → Scanner，其余 → MotionDevice |
| `MotionDevice::homeQ` | `home` attr | Q2Str（空格分隔） |
| `KinematicsBase::jointResolutions` | `resolutions` attr | Q2Str |
| `KinematicsBase::ikSolverType` | metadata `iksolver` | "None" / "Pieper" / "Jacobian" |
| `Scanner::Camera/Projector` | `<camera>/<projector>` | design + calibrated + frame 名 |
| `SceneObject` | `<obj>` | name/type/origin/parent_frame/children |
| `RigidObject` | `<obj type="rigid_object">` | origin + visual/collision |
| `Workcell::name` | `<workcell name>` | 站名 |

## 6. 内存级虚拟文件系统（VFS）

> **阶段一（本期，基础设施）**：本节（`Vfs` / `ZipVfs` / `DirectoryVfs` + `ZipArchive`
> 后端重载）优先实现，是场景序列化（阶段二）与打包（阶段三）的地基。

**核心认知：这个需求的本质是一个内存级虚拟文件系统（VFS）。** `workcell.xml`、`devices/robot.vdev`、
`geoms/base.bin` 就是"虚拟文件"，按 `/` 路径组织成一棵树；`save()` 把整棵树持久化成单个 zip，
`open()` 回读即加载，全程不落盘、不解压到目录。

经典参照：**physfs（PhysicsFS）** 正是这种抽象——把 zip 归档与真实目录统一成一套虚拟路径树
（Android APK / 游戏资源包同思路）。我们的场景是"内存为工作态、zip 为持久化态、真实目录为调试态"。

本设计把"序列化的载体"抽象成两层，与具体格式解耦：
- **`IMemoryVfs`**（= 内存级虚拟文件系统）：虚拟路径树接口（文件读写 / 资源绑定 / 保存 / 读取后端）；
- **`IEntity`**：可序列化实体基类，派生类重写自己的序列化（把模型写进 VFS 的虚拟文件）；
- **`ZipMemoryVfs`**：zip 后端（把内存树 save / open 成单个 zip）；`DirectoryVfs` 调试后端。

### 6.1 `IMemoryVfs`：内存 VFS 接口（虚拟路径 -> 数据）

```cpp
// vn::io 或 vn::robotics::io（归属见 6.4）；语义 = 内存级虚拟文件系统
class VN_IOBASE_API IMemoryVfs {
  public:
    virtual ~IMemoryVfs() = default;

    // ---- 路径 / 目录操作（VFS 语义）----
    virtual bool exists(const String& path) const = 0;
    virtual bool isFile(const String& path) const = 0;
    virtual bool isDirectory(const String& path) const = 0;
    /// 列出 dir 下的直接子项（文件与目录）。
    virtual std::vector<String> list(const String& dir = {}) const = 0;
    virtual bool remove(const String& path) = 0;

    // ---- 文件写入：虚拟路径 -> 数据 ----
    virtual bool writeFile(const String& path, const void* data, std::size_t size) = 0;
    virtual bool writeFile(const String& path, const std::vector<unsigned char>& data) = 0;
    virtual bool writeFile(const String& path, const String& text) = 0;      // UTF-8 文本
    /// 绑定：虚拟路径 -> 真实文件，save() 时才读入（不常驻内存）。
    virtual bool mountFile(const String& vfs_path, const std::filesystem::path& real_path) = 0;

    // ---- 文件读取 ----
    virtual bool readFile(const String& path, std::vector<unsigned char>& out) const = 0;

    // ---- 持久化后端（对上层透明）----
    virtual bool save(const std::filesystem::path& path) = 0;
    virtual bool save(std::vector<unsigned char>& out) = 0;
    virtual bool save(std::ostream& out) = 0;

    // ---- 工厂：打开现有包 / 目录 ----
    static std::unique_ptr<IMemoryVfs> openZip(const std::filesystem::path& path);
    static std::unique_ptr<IMemoryVfs> openZip(const void* data, std::size_t size);
    /// 调试：把真实目录作为后端（保存为文件夹而非 zip）。
    static std::unique_ptr<IMemoryVfs> openDirectory(const std::filesystem::path& dir);
};
```

> 命名：接口用 `I` 前缀（与 `INamed` / `IHierarchyNode` 一致），已定为 **`IMemoryVfs`**（语义：
> 内存级虚拟文件系统）；zip 实现 `ZipMemoryVfs`，目录调试后端 `DirectoryVfs`。

### 6.2 `IEntity`：可序列化实体基类（派生重写序列化）

> 模型类（Device/Workcell）**不实现** `IEntity`（不做自序列化，序列化逻辑在 `DeviceIO` /
> `WorkcellIO` / `XmlIOBase`）；`IEntity` 仅作扩展点，用于阶段三打包时的实体适配。

```cpp
class VN_IOBASE_API IEntity {
  public:
    virtual ~IEntity() = default;

    /// 把自身（及子实体）序列化进内存 VFS。
    virtual void serialize(IMemoryVfs& doc) const = 0;
    /// 从内存 VFS 恢复自身。
    virtual void deserialize(const IMemoryVfs& doc) = 0;
};
```

派生示例（自定义实体可重写序列化）：

```cpp
class FixtureEntity final : public IEntity {
  public:
    void serialize(IMemoryVfs& doc) const override {
        doc.writeFile(u8"fixtures/fixture1.xml", xml_bytes_);   // 自身 XML
        doc.mountFile(u8"fixtures/fixture1.stl", stl_path_);    // 绑定真实资源
    }
    void deserialize(const IMemoryVfs& doc) override {
        std::vector<unsigned char> xml;
        doc.readFile(u8"fixtures/fixture1.xml", xml);
        // ... 反序列化自身
    }
  private:
    std::vector<unsigned char> xml_bytes_;
    std::filesystem::path      stl_path_;
};
```

### 6.3 `ZipMemoryVfs`：zip 后端（VFS 的 zip 实现）

```cpp
// sdk/vine/robotics/io/ZipMemoryVfs.hpp
class VN_ROBOTICS_IO_API ZipMemoryVfs : public IMemoryVfs {
  public:
    ZipMemoryVfs();
    ~ZipMemoryVfs() override;
    // 全部纯虚实现，内部用 vn::io::ZipArchive 落地（见 6.5 后端补齐）

    // 建树后的持久化：save(path) / save(ostream&) / serialize()
    // 不再提供 openZip —— 打开已有包请用 ZipVfs::open(…, OpenMode::ReadOnly)（惰性只读，见 .ai/design/iobase-vfs-design.md §9.1）
};
```

> 调试后端 `DirectoryVfs`：把虚拟树直接映射到真实目录（`writeFile` -> 落盘文件，
> `openDirectory(dir)` 反向导入），与 zip 后端共用同一接口，便于断点 / 检查导出结果。

> 读取保证（现由 `ZipVfs` 承担）：`ZipVfs::open(…, OpenMode::ReadOnly)` 打开后**按需惰性读取单个条目**（libzip `zip_fopen` / `zip_fread`），
> 绝不把条目解压到磁盘；只有归档目录（名字/大小/类型）常驻内存，被读的条目才载入。
> 与 VMR `loadPkg`（`ZipDecompress` 解压到临时目录再读）相反。

### 6.4 模块归属（已定：放 `vn::io` / IOBase）

- **`IMemoryVfs` / `IEntity` / `ZipMemoryVfs` / `DirectoryVfs`**：全部放 `vn::io`（`IOBase`，
  与 `ZipArchive` 同层，导出宏 `VN_IOBASE_API`），通用可复用，`RoboticsIO` 与其它模块共享。
- **`DeviceIO` / `WorkcellIO`**（`RoboticsIO`）：使用上述抽象，不关心后端。
- 配套：`vn::io::ZipArchive` 增加少量后端重载（见 6.5），文件路径版保持不变。

### 6.5 后端补齐：`vn::io::ZipArchive`（IOBase 小改）

`ZipMemoryVfs` 用 `vn::io::ZipArchive` 落地。为支持"资源路径条目"与"保存到内存/流"，
给 `ZipArchive` 增加以下能力（文件路径版保持不变）：

```cpp
// 条目来自物理文件，saveAs() 时再读入（zip_source_file_create 懒读）
[[nodiscard]] IoError addFile(const String& path, const std::filesystem::path& real_path);
// 输出后端：内存字节 / std::ostream
[[nodiscard]] Result<std::vector<unsigned char>> toBytes() const;
[[nodiscard]] IoError saveAs(std::ostream& out) const;
// 条目读取与列目录：Zip::readEntry / Zip::entries（一次性操作；2026-09-23 从 ZipArchive 移到基础层 Zip）
```

要点：
- **内存态是核心**，流式 / 文件只是 `save()` / `open()` 的后端实现，对序列化代码透明。
- 资源路径条目在 `save()` 时才读盘，避免大 mesh 常驻内存；读入后同样 `zip_source_buffer` 入包。

### 6.6 打包 API 与流程（`RoboticsIO` 内，阶段三）

`DeviceIO` / `WorkcellIO` 增加 `loadPkg` / `savePkg`（见第 3 节类声明）。

**保存流程（无临时目录）**：
1. `saveXml` 生成内存 `XMLDocument`，`XMLPrinter` 输出字节；
2. 组装 `ZipMemoryVfs`：`writeFile("workcell.xml", ...)`；设备按引用形态写入——
   `.vdev` 写纯 XML 字节，`.vdevpkg` 把设备包自身（zip 字节）作为一个条目写入；
3. mesh：原生方案把内存中的顶点/法线/索引 `writeFile` 为 `geoms/xxx.bin`（见 6.8）；
   外部文件资源仍用 `mountFile(vfs_path, src_path)` 绑定真实路径；
4. `doc.save(pkg_path)` 一次成型（或 `save(std::vector<unsigned char>&)` 拿到内存包）。

**加载流程（无解压、绝不落盘）**：
1. `ZipVfs::open(pkg_path, OpenMode::ReadOnly)`（或接管字节的 `open(std::move(bytes), OpenMode::ReadOnly)`）就地打开 zip，**不抽取任何条目**；
2. `read("workcell.xml")` → 内存字节 → `tinyxml2::XMLDocument::Parse` 直接解析；
3. 设备对象按 `file` 属性取条目，**按扩展名分发**：
   - `.vdev`：`read` 取 XML 字节 → `DeviceIO::loadXml(String)` 直接解析；
   - `.vdevpkg`：`read` 取设备包字节 → `ZipVfs::open(bytes, OpenMode::ReadOnly)` 打开**嵌套 VFS**，
     读其中的 `device.xml` 解析，设备内部资源（geoms/...）相对嵌套根解析；
4. mesh 从包内 `geoms/xxx.bin` 读取并还原为 `TriangleMesh`/`IndexedTriangleMesh`（见 6.8）；
   包自包含；
5. 只按需读单个虚拟文件（`readFile` 惰性载入），**绝不先解压到目录**；
   无临时目录、无清理、无失败残留。

**包中包（嵌套 VFS）**：场景包内嵌设备包时，设备包自身就是 zip 字节条目；
`read` 取出后 `ZipVfs::open(bytes, OpenMode::ReadOnly)` 即得到一个独立的嵌套 VFS，与场景包互不干扰。资源引用按
"当前 VFS 根"解析：
- 场景包内 `workcell.xml` 引用 `devices/robot.vdevpkg` → 相对**场景包根**；
- 设备包内 `device.xml` 引用 `geoms/link1.bin` → 相对**设备包根**（嵌套 VFS）。

递归同构：设备包也可再含设备包，层级不限（实际场景一般两层）；每层都是同一个 `IMemoryVfs`
抽象，加载/保存代码不感知嵌套。

### 6.7 与其他格式的对比（HDF5 / CBOR / Zarr）

需求本质是"单文件**容器**：装多个命名条目（模型文档 + 资源）"，与 HDF5 / CBOR / Zarr 属不同层面：

| 格式 | 本质 | 与本设计的关系 |
|---|---|---|
| HDF5 | 层级容器（group/dataset + attribute），带类型 / 压缩 / 分块 | 概念最接近（单文件、多命名数据、层级），但面向科学数值数组、二进制不可读、libhdf5 依赖很重；可作为 `IMemoryVfs` 的另一个后端，不建议本期引入 |
| CBOR | 二进制**编码**（RFC 8949，类似二进制 JSON） | 不同轴：它替代的是 **XML**（编码单个文档），不解决"资源条目"；若想要二进制紧凑，未来演进是"容器不变 + XML 部分换成 CBOR" |
| Zarr | 分块压缩的 **N 维数组**格式（面向云计算 / 大数据） | 只面向数组数据，不适合"模型文档 + 异构资源"，不匹配 |

- **容器层面**的行业参照是 **ZIP/OPC、3MF、glTF `.glb`**（模型描述 + 二进制资源打包成单文件）；
  本设计的 `IMemoryVfs` 正是固定了这个"容器"概念，zip 只是其中一个实现。
- **编码层面**（XML vs CBOR）是正交的另一条轴：CBOR 对应物是 XML/JSON，不解决"资源怎么放"。
- 结论：**zip 容器 + XML 部分**成立；`IMemoryVfs` 保留换后端能力（目录 / 内存 / HDF5），
  XML→CBOR 可作为后续"单文件更紧凑"的演进路径，容器接口无需改动。

### 6.8 Mesh 存储（顶点 / 法线 / 索引）

Vine mesh 数据（`vn::geometry`）：

- `TriangleMesh`（非索引三角形汤）：`positions`（`Vec3fArray`）、可选 `normals`、可选 `texcoords`；
- `IndexedTriangleMesh`：`positions`、可选 `normals`、可选 `texcoords` + `indices`（`UInt32Array`）。

**元素约定（踩过一次，写死在这里）**：三个顶点数组是**每顶点一个元素** —— `Vec3fArray` 的 `size()` 是**顶点数**（不是 float 数，也不是 `3 × 顶点数`），
`indices` 是**每索引一个元素**。`Buffer<float>` 那一层才是 `3 × 顶点数`（`MeshBinSource` 里 `positions_->size() / 3u` 就是这么来的）。

**存储方案（2026-09-29）：一个网格一个文件，形式由内容决定**。不拆条（"一条数组一个条目"已经删掉，见本节末），也不做 OBJ 文本。

- **二进制 STL**（默认）：STL 装得下的都装它 —— 逐面法线 + 每个三角形三个顶点，没有索引（汤）。
- **`.vmesh`**（自描述单文件）：mesh 带 texcoords 时走这个，因为 STL 没有地方放 UV。判据就是这一条：`detail::stlWouldLose()`。

取舍（这条是刻意的）：STL 通用（包里的 `geoms/meshN.stl` 抽出来任何工具都能开）、更小（本资产未压缩 4.93 MB vs 7.31 MB）、代码少（没有版本/位/端序要维护）；代价是**共享顶点变成每三角形一份**（本资产 255415 → 296022 个顶点，+16%）、**索引结构丢失**（读回来是汤）、**逐顶点平滑法线只能留逐面**。带 UV 的模型通常是带贴图、带平滑法线、共享顶点多的模型，所以"有 UV 才走 `.vmesh`"这条规则正好把这三样一起接住了。

XML 描述只留一句指针（包内相对路径）：

```xml
<geometry>
  <!-- 一个 binary STL 文件：文件是汤，所以元素也写 triangle_mesh，且不写 vertex_count（STL 没有这个概念） -->
  <triangle_mesh triangle_count="4140" geometry="geoms/mesh0.stl"/>
  <!-- 带 texcoords 的网格走自描述形式，两个计数都写 -->
  <indexed_triangle_mesh vertex_count="12" triangle_count="4" geometry="geoms/mesh1.vmesh"/>
</geometry>
```

- 元素描述的是**文件**，不是内存里的形状：形状 `IndexedTriangleMesh` 但没 UV ⇒ 文件是 STL ⇒ 写 `<triangle_mesh>`。
- **形式由路径后缀决定**（`.stl` ⇒ STL reader，其余 ⇒ `.vmesh` reader，后缀**不分大小写** —— `.STL` 也是 STL），两个 reader 各自严格校验自己的规则 ⇒ 名字与内容不符 = **拒**，不猜。

`.vmesh` 布局（24 字节头 + 最多 4 段数组，段序固定：positions / normals / texcoords / indices，段内元素连续）：

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | magic | `"VMSH"`（`kMeshMagic`） |
| 4 | 2 | version | `2`（`kMeshVersion`） |
| 6 | 2 | flags | `MeshFlag`：**位 0..7 = 段**（0 positions、1 normals、2 texcoords、3 indices，位序 = 段序），**位 8..15 = 格式**（8 big-endian） |
| 8 | 8 | vertex_count | 顶点数：positions/normals/texcoords 各这么多元素 |
| 16 | 8 | triangle_count | 三角形数：索引段有 `3 × triangle_count` 个 `uint32` |

- 元素：`Vec3f`（12 字节）每顶点、`Vec2f`（8 字节）每顶点、`uint32`（4 字节）每索引 —— 就是 `Array.hpp` 的打包布局，段可以整块读。
- **端序 = 写它的那台机器的端序**，`flags` 位 8 记下来；读的时候与当前主机不同就直接**拒绝**（`parseMeshHeader()` 返回 false），
  而不是按错的端序解释数字（错的数字比明确的拒绝更糟）。
- 自描述：`MeshHeader::byteCount()` 由 `flags` 与两个计数算出（`24 + Σ 段字节`），
  所以“头声明的长度 ≠ 条目实际长度”也被拒（后面多挂一条尾巴同样算不一致）。
- 计数先按元素大小夹住再分配（`total / sizeof(T)`），损坏的头不可能要求比条目更大的内存。

二进制 STL 布局（`kStlPrefixSize` / `kStlTriangleSize`）：

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0 | 80 | 自由头 | 文本随写，但**不能以 `solid` 开头**（那是文本形的判据）；本模块写 `"binary STL written by vine::robotics::io"` |
| 80 | 4 | triangle_count | `uint32` |
| 84 | 50 × n | 三角形 | 逐面法线 3×float32、三个顶点各 3×float32、2 字节属性计数（写 0） |

- **固定小端**（STL 的定义如此）——本模块唯一不写“自己机器的端序”的地方，大端主机由 `putU32Le()` 显式换序（这条在本机无法实测，只能靠代码与变异验证）。
- 自描述且**长度唯一确定**：`84 + 50 × n == 条目长度`，不符即拒（截断、多挂尾巴、计数改错都被这一条拦住）；`n == 0` 也拒（没有三角形的文件不是 mesh）。
- **法线保留模型自己的值**：一个三角形的三个角法线一致时（容差 `kSameNormalTolerance = 1e-4`）写该值，否则由几何算。
  重算会**悄悄改掉模型说的法线**，甚至按缠绕方向翻面 —— 本资产的 98448/98674 个三角形逐位一致、226 个在 1e-4 内一致（数据是从 STL 那类流程来的）。
- 文本形 STL 不读：长度对不上 ⇒ 拒。

**写入侧**（`XmlIOBase::exportGeometry` → `writeMeshFile()` → `detail::MeshBinSource` / `detail::StlMeshSource`）：
- 网格数据已在内存（模型持有 `Vec3fArray`/`UInt32Array`）→ 条目直接指向它们的 `Buffer`，不拷字节。
- **写入不改写为字节块（2026-09-29 落地）**：两个源都自持 `intrusive_ptr<const Buffer<T>>`，拉一次吐头 + 各数组段；源自持 buffer，所以“包延迟到 save 才拉”时也不需要调用方保住什么。
  早期实现是“把数组拷成 `std::vector<unsigned char>` 再 `writeFile(bytes)`”，大 mesh 下是**两份**拷贝（byte 块 + 后端条目）。
  注意：**别用 `Fragment` 借 `mesh->positions()` 这种 view** —— 延迟后端在 save 时才拉，调用方一旦释放 mesh 就是悬垂（见 iobase 设计文档 §8.5 的判据）。
- 长度都精确可报（`MeshHeader::byteCount()` / `kStlPrefixSize + kStlTriangleSize × n`），所以 ZIP 路径拿到的仍是“知道长度的源”。
- **段与头必须同源**：每个数组位由**实际写进 `blocks_` 的那一段**设置。曾经出现过“positions 位设了、段没进 `blocks_`”
  （头报 72、实际只吐 36 字节，读侧 `InvalidData`）——现在头与段不可能对不上。
- **一个网格只过一道闸门：`detail::MeshArrays::take()`**（2026-09-29 复核后收口）。它取数组时就查完四件事：顶点数组是整数个顶点（`size % 3 == 0`）、
  索引是整数个三角形、normals/texcoords 要么没有要么正好每顶点一个（"**空数组 = 没有这个数组**"，因为 `Mesh::positionsBuffer()` 这类访问器**永不返回 null**，未设置时是空），
  以及**每个索引都指向一个存在的顶点**（`cornersAreInRange()`）。任何一条不成立 ⇒ 两个写源都拒（`IoError::InvalidData` ⇒ `savePkg` 抛），
  而不是"把不合格的那个数组丢掉、其余照写"——那是对内容的静默修改（早先 `.vmesh` 就这么干，而 STL 直接拒，两条路对同一个网格给出不同答案）。
  角点检查因此只剩**一处**（两个源里各自的那份已删），`MeshBinSource::addBlock()` 的长度核对退化为**第二道防线**
  （变异显示它已不能单独被触发 —— "守卫存在"不等于"守卫被用例钉住"，见记忆里那条）。
- 去重：同一 mesh 被多个 visual/collision 引用时只写一次（`ExportContext::mesh_paths`，按 `Shape` 指针）。
- 条目组织：`geoms/meshN.stl` 或 `geoms/meshN.vmesh`（`writeMeshFile()` 的 `geom_seq` 计数），与 XML 的 `geometry=` 相对路径对应。

**读取侧**（`XmlIOBase::parseGeometry` → `detail::meshFromFile()`）：
- `readMeshBin()`（`.vmesh`）：先读完头、按条目长度校验计数，再逐段读进 `Vec3fArray`/`Vec2fArray`/`UInt32Array`（`MeshBinData`）；
  然后**查一遍索引是否都落在顶点范围内**（写侧拒产、读侧拒收，同一条规则的两端 —— 一个索引越界的网格下游没法安全读）；
  最后 `meshFromBin()` 按 `indexed` 建 `IndexedTriangleMesh` / `TriangleMesh`。
- `readStl()`（`.stl`）：读 84 字节头，用文件自己的计数核对长度，再按 50 字节记录（交错：法线在前）拆进 positions/normals —— 读出来就是汤（每顶点一个 `Vec3f`、法线逐面重复三份、无索引），最后 `meshFromStl()` 建 `TriangleMesh`；描述若说 `indexed_triangle_mesh` 则**拒**（文件不支持这个承诺）。
- 任何一处不符即返回空 —— 损坏的文件让形状**缺掉**，而不是“能读多少算多少”地变成更小的 mesh。
- 条目直接流入目标数组：`ArraySink<T>` / `readBlock()`（私有，`IoUtils.hpp`）实现 `vn::io::DataSink`，字节数不落在元素边界上则报 `InvalidData`。
  更早的实现是“先读成 `std::vector<unsigned char>` 再转数组”，多一份整文件拷贝。`IoUtils.hpp` 里用 `static_assert` 把 `sizeof(Vec3f) == 12` / `sizeof(Vec2f) == 8` 钉住 —— **这条一改，`.vmesh` 格式就变了**。

**拓展策略（2026-09-29 定，先写死规矩，不加机制）**：
- 位权分工：**位 0..7 是段**（位序 = 段序，新段取下一个空位）；**位 8..15 是格式位**（说的是"怎么读这一段"，不是"有什么段"）。
  端序位本来在 1<<4（混在段位里），趁仓库里还没有任何已提交的 v2 文件挪到 **1<<8** —— 之后就再不动它。
- `version` 只在"**老 reader 必须拒**"时才 +1。新段一律**追加位、不复用、不重排**：位不变则老 reader 的行为只有一种 ——
  拒（`byteCount()` 只数认得的位，`header.byteCount() != total` 直接 false），**不做前向容忍**（`.vmesh` 只活在包里，写读同仓；误读比拒绝贵）。
- 因此已知的因果：**加一个同类定点属性**（颜色 / 切线 / 二套 UV）⇒ 追加一个段位，`version` 不变；**改索引宽度 / 子网格 / LOD / 材质分组 / 要随机访问** ⇒ `version 3`；
  段位用满 8 个或需要"任意命名属性"时才把"位 = 段"换成 TLV 表。
- **未知位 ⇒ 拒**：`parseMeshHeader()` 拿 `header.flags` 比 `kMeshKnownFlags`，多一位就 false —— `flags` 说了什么就必须在文件里兑现，
  不能"跳过去当没看见"（一个长度算不出来的段没法跳过，半读的 mesh 正是头存在的意义）。
- **STL 那一侧没有版本位**（标准格式，改不动）：形式的变化只能靠**路径**（换后缀）或走 `.vmesh`。这也是留给"下一次大改"的同一根杠杆：
  写 `geoms/meshN.vmesh3` 而 `geometry=` 照旧、两个 reader 并存 —— 头布局不必背一辈子的兼容债。
- 不做的（等真消费者）：`.vmesh` 头里放段偏移表（换"只读 positions"/mmap 随机访问，今天没有部分读的消费者）、每段压缩/分块（外层包本来就是 ZIP）、LOD/子网格。

**v1 拆条形态（历史登记：三文件形式，已删除）**：
- 曾经：每个 mesh **三个裸 `.bin` 条目**（`meshN.positions.bin` / `.normals.bin` / `.indices.bin`，宿主端序、无头），XML 用 `positions=`/`normals=`/`texcoords=`/`indices=` 四个属性指过去。
- 2026-09-29：**读取代码整段删除**（`parseGeometry` 的四属性分支、`meshCountsAgree()`、`getAttrCount()`、`readTypedArray()`）。
  它唯一的使用者是 `test_data/robots/irb_1600_10_145.vdev`，该资产已**转换**成 STL 形态（几何逐位不变、法线保留原值、mesh 元素之外的 XML 逐字节不变，见 `test_data/robots/README.md`），仓库里不再有任何 v1 文件。
- 记一次教训：这个形态的 `vertex_count`/`triangle_count` 校验曾经按“flat float 数组”写（`positions.size() % 3`），把所有真实 v1 包判成不一致 —— 元素约定是“**每顶点一个元素**”，见本节开头。

### 6.9 BRep / STEP（本期不实现，设计预留）

`vn::geometry::BrepShape` 以非拥有 `TopoDS_Shape*` 持有 OCCT 实体（SDK 头只前向声明、不引 OCCT）；
`brepio::BrepLoader` 的 STEP/IGES 解析仍是 TODO（需引入 OpenCASCADE）。

**本期不实现 BRep**：`XmlIOBase::parseGeometry / exportGeometry` 对 `BrepShape` 跳过（或记警告），
模型改动与测试均不涉及 OCCT。

**设计预留**（保证后续平滑加入）：
- **XML 描述**：预留 `<brep>` 子元素：

  ```xml
  <geometry>
    <!-- 预留：BRep 本期不实现（见 6.9） -->
    <brep source="breps/part1.brep"/>
  </geometry>
  ```

- **存储**：与 mesh 同属"二进制大块"，走 `IMemoryVfs` 条目；候选编码（实现时选一）：
  - OCCT 原生 BRep 二进制：`BRepTools::Write(shape, std::ostream&)` / `Read(shape, std::istream&)`
    —— 无损、快、天然支持内存流（契合"用 OCCT 序列化到内存流"）；
  - STEP 文本（`STEPControl_Writer`）—— 可跨 CAD 互操作，但体积大、慢；
  - 阶段 A 占位：原 `.stp` 文件字节作不透明条目（`mountFile(vfs_path, src_path)`）。
- **依赖**：OCCT 是重依赖（体积 / 构建成本高），预留为**可选后端**；接入点只在
  `IoUtils` / `XmlIOBase` 的几何分支 + `brepio::BrepLoader`，不进入本期链接。

### 6.10 复用与去重（设备实例 / mesh 共享）

**设备实例复用**：
- `workcell.xml` 中 `<obj type="device" file="devices/robot.vdev">` 是**定义引用**；实例差异
  （name / origin / parent_frame / home / q / resolutions）都在 `<obj>` 上。多个实例 = 多个
  `<obj>` 指向同一 `file`。
- **导出去重**：`WorkcellIO::ExportContext` 维护 `内容指纹 -> 包内相对路径` 映射
  （参考 VMR `VByteSequenceFingerprint` + `all_exported_devs`）；同一份设备定义只写一次
  `devices/xxx.vdev`（或 `.vdevpkg`），后续实例复用同一路径。
- 指纹来源：设备**记住源文件**（`Device::setFilePath` / `filePath()`，端口 VMR `getFilePath`）；
  无源文件（内存构建）时用序列化 XML 的内容指纹。
- **导入**：每个实例按 `file` 各自加载定义（每实例独立 `DeviceData`），不做跨实例共享——
  避免共享可变状态，简单可靠。

**Mesh 复用**：
- `XmlIOBase::ExportContext::all_exported_shapes`（`map<const Shape*, 包内路径>`）按形状
  去重：同一 mesh 只写一次 `geoms/xxx.bin`，多个 visual/collision 的 `<geometry>` 引用同一路径
  （见 6.8）。
- 多个设备实例共享同一设备定义时，其 mesh 也自动只存一次（定义只写一次）。

## 7. 需要的模型改动（阶段二：场景序列化的前置）

1. **`DeviceMetadata` 扩展**（`Device.hpp`，workcell 模块）：
   - 新增 `String create_time; String modified_time;`
   - 新增 `workcell::LengthUnit length_unit{ workcell::LengthUnit::Millimeter };`
   - 新增 `kinematics::IKSolverType ik_solver_type{ kinematics::IKSolverType::Iterative };`
     （workcell 已依赖 kinematics 的 Q/State/Frame，无循环问题）
2. **新增 `workcell::LengthUnit`**：`enum class LengthUnit { Meter = 1, Millimeter = 1000 };`
   （放 workcell 模块，IO 依赖 Core；复用 meshio 的枚举会引入对 meshio 的依赖，不采用）
3. **`DeviceData` 新增 `DeviceKind kind{ DeviceKind::Other };`**，`Device::initDevice` 里
   `setDeviceKind(data->kind)`。这样加载时为 MotionDevice 设置 Manipulator/ExternalAxis/Positioner 等 kind。
4. **`MotionDevice` 加载后设置**：`setHomeQ(...)`（已有）、`kinematics()->setIKSolverType(...)`（已有）。
5. **内部工具 `IoUtils`**：`Q2Str`/`Str2Q`、`poseToXml`/`xmlToPose`（四元数）、
   `double`↔字符串（定精度）、材质/形状按名注册表与查重、mesh 数组↔bin 缓冲
   （`Vec3fArray`/`UInt32Array` ↔ 字节，含端序与计数校验）。
6. **`Device` 记住源文件**：新增 `setFilePath` / `filePath()`（端口 VMR `getFilePath`），
   用于导出时设备定义去重（见 6.10）。

## 8. 构建与测试

**阶段一（本期，VFS 基础设施）**——`tests/test_iobase/`（`VfsTest`，IOBase 级组件）：
  1. `VfsTest`：
     `writeFile`/`readFile`/`mountFile` 往返；`exists`/`isFile`/`isDirectory`/`list`/`remove`
     语义正确；`ZipMemoryVfs` 与 `DirectoryVfs` 对同一虚拟树 `save`/`open` 结果一致；
     `open(std::move(bytes), OpenMode::ReadOnly)` 内存包与文件版一致。

**阶段二（场景序列化）**——`tests/test_robotics_io/`（链接 `vn::RoboticsIO`）：
  2. `DeviceIOTest`：
     - MotionDevice round-trip（2 关节）：`dof()`、`joints().size()`、bounds/home/ik 类型一致；
     - Scanner round-trip：`cameras()/projectors()` 内参一致、frame 绑定恢复；
     - 单位缩放 mm↔m：原点平移与 near/far 正确缩放；
     - 错误路径：版本不支持、缺 name/kind、link 重名、缺关节 → 抛异常。
  3. `WorkcellIOTest`：
     - 设备 + 刚体 + 多层 children round-trip：对象名、父级关系、`parentObject()` 复原；
     - `parent_frame` 挂到设备末端帧 round-trip（`baseFrame()->parent()` 帧名一致）；
     - 设备 `home`/`q`/`resolutions` 往返一致。

**阶段三（打包）**：
  4. `PkgIOTest`（基于 VFS，不落盘）：`savePkg` → `loadPkg` 等价于 `saveXml` → `loadXml`；
     包内路径正确（workcell.xml / devices / geoms）；`mountFile` 绑定的资源在 save 时读入；
     `save(std::vector<unsigned char>&)` 内存包与文件版结果一致；
     **嵌套设备包**：场景包内嵌 `.vdevpkg`，加载按扩展名分发、`ZipVfs::open(…, OpenMode::ReadOnly)` 打开嵌套 VFS，
     设备资源相对设备包根解析，与场景包根互不干扰；
     **复用去重**：两个设备实例指向同一 `file` → 设备定义只写一次；共享 mesh 只存一次，
     多处 `<geometry>` 引用同一 `geoms/xxx.bin`。
- 新增 `.cpp` 后需 `cmake -S . -B build`（`file(GLOB_RECURSE)` 无 `CONFIGURE_DEPENDS`）。

## 9. 后续阶段

**阶段二：场景序列化**（VFS 基础设施 OK 后做，见第 3~5、7 节）：
- `RoboticsIO` 新增 `XmlIOBase` / `DeviceIO` / `WorkcellIO`（`loadXml` / `saveXml`），基于 VFS 落地；
- 模型改动：`DeviceMetadata` 扩展（create_time / modified_time / length_unit / ik_solver_type）、
  `workcell::LengthUnit`、`DeviceData::kind`、`IoUtils`（见第 7 节）；
- tinyxml2 以 FetchContent 静态烤入 `viRoboticsIO*.dll`。

**阶段三：打包**（场景序列化 OK 后做）：
- `DeviceIO` / `WorkcellIO` 增加 `loadPkg` / `savePkg`（`.vdevpkg` / `.vwspkg`，直接写 zip 流，
  无临时目录、不解压到目录）；
- mesh 阶段 B：原生 bin + XML 描述（顶点/法线/索引，见 6.8）。

**其它后续**：
- Mesh 阶段 A 占位：只作不透明字节条目（`mountFile`），不解析顶点（见 6.8）。
- BRep / STEP（见 6.9）：引入 OpenCASCADE 后实现 `<brep>` 解析/导出（OCCT 序列化到内存流，
  `BRepTools::Write/Read`），或阶段 A 作不透明 `.stp` 条目。
- `SaveOptions.ignore_part` 等过滤选项。
- 任意可写流（回调式 `zip_source_function`）：当前流式重载已覆盖内存缓冲与 `std::ostream`。

## 10. 决策记录

**已定：**
- [x] 命名：`DeviceIO` / `WorkcellIO` / `XmlIOBase`（去 V 前缀）；VFS 命名 `IMemoryVfs` /
      `ZipMemoryVfs` / `DirectoryVfs`（接口用 `I` 前缀）。
- [x] 新模块 `RoboticsIO` 独立 DLL（`src/robotics/io`），依赖 `vn::RoboticsCore vn::Geometry
      vn::IOBase`，tinyxml2 以 FetchContent 静态烤入。
- [x] VFS 归属 `vn::io`（IOBase）、接口范围**完整 VFS**（`exists`/`isFile`/`isDirectory`/
      `list`/`remove` + `DirectoryVfs` 调试后端）。
- [x] 实施顺序：**VFS 基础设施优先（阶段一）**，再做场景序列化（阶段二）、打包（阶段三）；
      打包（`.vdevpkg` / `.vwspkg`）在场景序列化 OK 后再实现（见第 9 节）。
- [x] 模型类**不继承** `IEntity`（不做自序列化）：序列化在 IO 层（`DeviceIO` / `WorkcellIO` /
      `XmlIOBase`）；`IEntity` 仅作阶段三打包扩展点。

**待定（阶段一实现前确认）：**
- [ ] 设备文件引用 vs 内联嵌入：本设计采用 **VMR 风格文件引用**（复用、文件小）。是否接受？
      （或要求单文件自包含则改内联。）
- [ ] 姿态表示：采用**四元数**（无损、`Isometry3d` 原生）。是否也要兼容 RPY 读取？
