#pragma once
#include <QDockWidget>
#include <QPoint>
#include <QSize>
#include <QString>

#include <vine/String.hpp>

#include <vine/appfw/gui/Gui.hpp>

VN_APPFWGUI_NS_BEGIN

class Convert {
  public:
    static Point  toPoint(const QPoint& pt);
    static QPoint toQPoint(const Point& pt);

    static Size  toSize(const QSize& pt);
    static QSize toQSize(const Size& pt);

    static Qt::DockWidgetAreas toQDockAreas(DockAreas areas);
    static DockAreas           toDockAreas(Qt::DockWidgetAreas qareas);

    static QDockWidget::DockWidgetFeatures toQDockFeatures(DockFeatures features);
    static DockFeatures                    toDockFeatures(QDockWidget::DockWidgetFeatures qfeatures);

    /**
     * @brief Converts a framework string to a Qt string.
     *
     * @param s Framework string to convert.
     * @return The equivalent Qt string.
     */
    static QString toQString(const String& s);

    /**
     * @brief Converts a Qt string to a framework string.
     *
     * @param qs Qt string to convert.
     * @return The equivalent framework string.
     */
    static String fromQString(const QString& qs);
};

VN_APPFWGUI_NS_END
