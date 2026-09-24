#include "Convert.hpp"


VN_APPFWGUI_NS_BEGIN

Point Convert::toPoint(const QPoint& pt)
{
    return Point(pt.x(), pt.y());
}

QPoint Convert::toQPoint(const Point& pt)
{
    return QPoint(pt.x, pt.y);
}

Size Convert::toSize(const QSize& s)
{
    return Size(s.width(), s.height());
}

QSize Convert::toQSize(const Size& s)
{
    return QSize(s.x, s.y);
}

Qt::DockWidgetAreas Convert::toQDockAreas(DockAreas areas)
{
    Qt::DockWidgetAreas qareas = Qt::DockWidgetArea::NoDockWidgetArea;
    if (testFlag(areas, DockAreas::Left)) {
        qareas |= Qt::DockWidgetArea::LeftDockWidgetArea;
    }
    if (testFlag(areas, DockAreas::Top)) {
        qareas |= Qt::DockWidgetArea::TopDockWidgetArea;
    }
    if (testFlag(areas, DockAreas::Right)) {
        qareas |= Qt::DockWidgetArea::RightDockWidgetArea;
    }
    if (testFlag(areas, DockAreas::Bottom)) {
        qareas |= Qt::DockWidgetArea::BottomDockWidgetArea;
    }
    return qareas;
}

DockAreas Convert::toDockAreas(Qt::DockWidgetAreas qareas)
{
    DockAreas areas = DockAreas::None;
    if (qareas.testFlag(Qt::DockWidgetArea::LeftDockWidgetArea)) {
        areas |= DockAreas::Left;
    }
    if (qareas.testFlag(Qt::DockWidgetArea::TopDockWidgetArea)) {
        areas |= DockAreas::Top;
    }
    if (qareas.testFlag(Qt::DockWidgetArea::RightDockWidgetArea)) {
        areas |= DockAreas::Right;
    }
    if (qareas.testFlag(Qt::DockWidgetArea::BottomDockWidgetArea)) {
        areas |= DockAreas::Bottom;
    }
    return areas;
}

QDockWidget::DockWidgetFeatures Convert::toQDockFeatures(DockFeatures features)
{
    QDockWidget::DockWidgetFeatures qfeatures = QDockWidget::DockWidgetFeature::NoDockWidgetFeatures;
    if (testFlag(features, DockFeatures::Closable)) {
        qfeatures |= QDockWidget::DockWidgetFeature::DockWidgetClosable;
    }
    return qfeatures;
}

DockFeatures Convert::toDockFeatures(QDockWidget::DockWidgetFeatures qfeatures)
{
    DockFeatures features = DockFeatures::None;
    if (qfeatures.testFlag(QDockWidget::DockWidgetFeature::DockWidgetClosable)) {
        features |= DockFeatures::Closable;
    }
    return features;
}

QString Convert::toQString(const String& s)
{
    auto u16 = s.toUtf16();
    return QString::fromStdU16String(u16);
}

String Convert::fromQString(const QString& qs)
{
    return String::fromUtf16(reinterpret_cast<const char16_t*>(qs.utf16()), qs.size());
}

VN_APPFWGUI_NS_END
