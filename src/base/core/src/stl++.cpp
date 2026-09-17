#include <iostream>
#include <vine/String.hpp>
#include <vine/stl++.hpp>

V_CORE_NS_BEGIN
std::ostream& operator<<(std::ostream& cout, const String& str)
{
    cout << str.std_str_view();
    return cout;
}

V_CORE_NS_END
