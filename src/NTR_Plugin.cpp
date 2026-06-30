#include "NTR_Plugin.hpp"

Plugin* pluginInstance;

__attribute__((visibility("default")))
void init(Plugin* p) {
    pluginInstance = p;

    p->addModel(modelWabot);
}
