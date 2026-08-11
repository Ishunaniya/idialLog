// app_context.cpp — 应用上下文唯一实例
#include "app_context.h"

namespace dl {

AppContext& App() {
    static AppContext context;
    return context;
}

} // namespace dl
