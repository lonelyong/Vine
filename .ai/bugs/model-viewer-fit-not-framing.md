# Bug: 模型查看器打开文件后不对准内容，适应屏幕把模型放到缩不回来

- **日期**: 2026-10-26
- **模块**: plugins/model_viewer + viz/graphics
- **状态**: 已修复

## 现象

用 `open_model_file` 打开一个毫米级 STL（约 1000×1000×510，原点不在模型中心）后：

- 初始相机没有对准内容：模型占满画面，看不到整体；
- 右键菜单"适应屏幕"之后更夸张：模型被放得特别大，滚轮几乎缩不回来。

## 根因

两层，叠在一起：

1. `ModelPickManipulator` 造内层 `OrbitCameraManipulator` 时**没有传场景**（`OrbitCameraManipulator(camera)`）。
   `fitToScreen()` 靠 `scene->boundingBox()` 取景，没有场景就对空指针返回 false —— 于是
   `SurfaceWindow::fitToScreen()` 退到 `home()`，也就是"造操纵器那一刻的相机"：默认 `eye=(0,0,5)`、
   `target=原点`。毫米级模型半对角线约 750，默认站位自然显得巨大；滚轮步长按当前距离比例放大，从 5 起步要上千格才有变化。
2. `fitToScreen()` 只改距离，不改相机近/远平面。相机默认 `far=1000`，而这个模型的取景距离约 2200 ——
   即使取景成功，模型也会整个落在远平面之外（画面为空）。

## 修复

1. `ModelPickManipulator` 的构造函数收 `scene` 并交给内层 `OrbitCameraManipulator`；
   `ModelRenderView::buildScene()` 传入 `view_->scene()`。
2. `OrbitCameraManipulator::fitToScreen()` 按取景距离缩放近/远平面（`near = 1% d`、`far = 100x d`，
   保持相机默认 1:10000 的深度比）；透视与正交两条分支都处理。

## 验证

- `tests/test_gui/ModelViewerTest.cpp` 的 `ThePickManipulatorFramesTheSceneItDraws`：毫米级（边长 2000）立方体，
  断言 `fitToScreen()` 返回 true、相机对准包围盒中心、距离随内容规模、`near < d - r` 且 `far > d + r`。
  把场景参数去掉后，该用例在 `fitToScreen()` 断言处失败（已实测）。
- `test_gui`、`test_graphics`、`test_vsg` 全绿。
