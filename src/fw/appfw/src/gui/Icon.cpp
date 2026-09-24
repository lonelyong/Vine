#include <vine/appfw/gui/Icon.hpp>

#include <QIcon>

VN_APPFWGUI_NS_BEGIN

struct Icon::Impl {
    QIcon qicon;
};

Icon::Icon()
    : d(new Impl())
{
}

Icon::Icon(const QIcon& qicon)
    : d(new Impl())
{
    d->qicon = qicon;
}

Icon::Icon(const String& path)
    : d(new Impl())
{
    auto utf16 = path.toUtf16();

    d->qicon = QIcon(QString::fromStdU16String(utf16));
}

Icon::Icon(const Icon& other)
    : d(new Impl())
{
    d->qicon = other.d->qicon;
}

Icon& Icon::operator=(const Icon& other)
{
    if (this != &other) { d->qicon = other.d->qicon; }

    return *this;
}

Icon::~Icon() = default;

const QIcon& Icon::value() const
{
    return d->qicon;
}

bool Icon::isNull() const
{
    return d ? d->qicon.isNull() : true;
}

Icon::operator QIcon() const
{
    return d->qicon;
}

VN_APPFWGUI_NS_END
