#pragma once

#include "Object.hpp"
#include "Signal.hpp"

VN_CORE_NS_BEGIN

class VN_CORE_API EventArgs : public Object {
    VN_OBJECT_META_DECL
  public:
};

template <typename TVal>
class PropertyChangedEventArgs : public EventArgs {
    VN_OBJECT_META_DECL;

  public:
    // using ValType   = typename std::remove_reference<typename std::remove_const<TVal>::type>::type;
    // using ValTypeCR = typename std::add_lvalue_reference<typename std::add_const<ValType>::type>::type;

    using ValType = TVal;

  public:
    PropertyChangedEventArgs(ValType new_val, ValType old_val)
      : new_val_(new_val)
      , old_val_(old_val)
    {}

  public:
    ValType newValue() const
    {
        return new_val_;
    }

    ValType oldValue() const
    {
        return old_val_;
    }

  private:
    ValType new_val_;
    ValType old_val_;
};

template <typename TSender, typename TEventArgs = EventArgs>
requires std::is_base_of_v<EventArgs, TEventArgs>
using Event = Signal<TSender&, TEventArgs&>;

VN_TMPL_OBJECT_META_IMPL(template <typename TVal>, PropertyChangedEventArgs<TVal>, EventArgs);

VN_CORE_NS_END
