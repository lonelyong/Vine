#pragma once

#include <vector>

#include <QGridLayout>
#include <QLabel>
#include <QString>
#include <QWidget>

#include <vine/Events.hpp>
#include <vine/String.hpp>
#include <vine/appfw/Document.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/gui/Control.hpp>

#include "ModelDocument.hpp"

namespace vn::model_viewer
{

/**
 * @brief 模型信息面板：显示**当前文档**的类型、标题、顶点数与三角形数。
 *
 * 它是 `Control`（appfw 的 UIElement 包装）而不是裸 QWidget：dock 面板要的是 UIElement。整个类 header-only、
 * 不带自己的元数据、也没有信号槽 —— 于是 GUI 用例可以直接 include 它、驱动它，不必先加载插件。
 *
 * "内容跟当前文档"这件事由**它自己**接上（followDocumentManager）：这是 appfw-document-model.md §12 里
 * "面板自己的内容自己听"那一档 —— 它只碰自己的控件，不碰 ribbon、也不碰别人的面板，所以不需要宿主替它编排。
 * 宿主（这个插件）只负责建它、挂上 dock。
 */
class ModelInfoPanel : public vn::appfw::gui::Control {
  public:
    /**
     * @brief 建面板。
     *
     * @param parent 传给内部那个 QWidget 的父窗口；被 dock 接管之后由 dock 重父化。
     */
    explicit ModelInfoPanel(QWidget* parent = nullptr)
      : Control(new QWidget(parent))
    {
        auto* root   = impl<QWidget>();
        auto* layout = new QGridLayout(root);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setHorizontalSpacing(12);
        layout->setVerticalSpacing(6);

        type_label_      = new QLabel(root);
        title_label_     = new QLabel(root);
        vertices_label_  = new QLabel(root);
        triangles_label_ = new QLabel(root);

        int row = 0;
        addRow(root, layout, row, u8"类型", type_label_);
        addRow(root, layout, row, u8"标题", title_label_);
        addRow(root, layout, row, u8"顶点数", vertices_label_);
        addRow(root, layout, row, u8"三角形数", triangles_label_);
        layout->setRowStretch(row, 1);

        clear();
    }

  public:
    /**
     * @brief 让面板跟着某个文档管理器的**当前文档**走。
     *
     * 只接 `currentChanged`：`opened` 不改当前选择，而当前文档被关掉时管理器会先发一次 `currentChanged(nullptr)`。
     * 连接是 RAII 的（`~Connection()` 就断开），所以面板被销毁时它自己就摘干净了。
     *
     * @param documents 要跟随的管理器（借用；必须比面板活得久）。
     */
    void followDocumentManager(vn::appfw::DocumentManager& documents)
    {
        connections_.push_back(documents.currentChanged.connect(
            [this](vn::appfw::DocumentManager& owner, vn::appfw::DocumentEventArgs&) { setDocument(owner.current()); }));

        setDocument(documents.current());
    }

    /**
     * @brief 显示一份文档。
     *
     * @param document 要显示的文档；nullptr 表示"没有当前文档"（回到空状态）。不是模型文档时照样显示它的类型与
     *                 标题，两个数则说明"不是模型文档" —— 面板宁可说清楚，也不假装那是 0。
     */
    void setDocument(const vn::appfw::Document* document)
    {
        if (document == nullptr) {
            clear();
            return;
        }

        // 文档类型今天还没有自己的元数据（§5.7），所以按注册用的 id 认它；id 与类的一致性本来就由
        // registerType() / adopt() 保证。将来文档带 meta 之后，这里换成 obj_cast<ModelDocument>(document) 即可。
        const bool           is_model = document->typeId() == vn::String(ModelDocument::kTypeId);
        const ModelDocument* model    = is_model ? static_cast<const ModelDocument*>(document) : nullptr;

        type_label_->setText(textOf(document->typeId()));
        title_label_->setText(textOf(document->title()));
        vertices_label_->setText(model != nullptr ? QString::number(static_cast<qulonglong>(model->vertexCount()))
                                                  : notAModelText());
        triangles_label_->setText(model != nullptr ? QString::number(static_cast<qulonglong>(model->triangleCount()))
                                                   : notAModelText());
    }

    /// 回到"没有当前文档"的状态。
    void clear()
    {
        type_label_->setText(noneText());
        title_label_->setText(noneText());
        vertices_label_->setText(noneText());
        triangles_label_->setText(noneText());
    }

  public:
    /// 面板上四行的当前文本（用例断言它们，而不是去翻 QLabel 的层级）。
    QString typeText() const { return type_label_->text(); }
    QString titleText() const { return title_label_->text(); }
    QString verticesText() const { return vertices_label_->text(); }
    QString trianglesText() const { return triangles_label_->text(); }

  private:
    /// 加一行"名字 + 值"：名字是静态文本，值是面板要显示的东西。
    static void addRow(QWidget* root, QGridLayout* layout, int& row, const char8_t* name, QLabel* value)
    {
        auto* caption = new QLabel(QString::fromUtf8(reinterpret_cast<const char*>(name)), root);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(caption, row, 0, Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(value, row, 1);
        ++row;
    }

    /// vn::String 是 UTF-8 的，按字节交给 Qt 即可。
    static QString textOf(const vn::String& text)
    {
        return QString::fromUtf8(reinterpret_cast<const char*>(text.data()), static_cast<int>(text.size()));
    }

    static QString noneText() { return QStringLiteral("—（没有当前文档）"); }
    static QString notAModelText() { return QStringLiteral("—（不是模型文档）"); }

  private:
    QLabel*                     type_label_      = nullptr;
    QLabel*                     title_label_     = nullptr;
    QLabel*                     vertices_label_  = nullptr;
    QLabel*                     triangles_label_ = nullptr;
    std::vector<vn::Connection> connections_;
};

} // namespace vn::model_viewer
