#pragma once

#include "../modules/Module.h"

namespace peare {
namespace resources {

class IResourceResolver {
public:
    virtual ~IResourceResolver() = default;
    virtual const ResourceEntry* find(const QString& type,
                                      const QString& name,
                                      const QString& preferredLanguage) const = 0;
};

} // namespace resources
} // namespace peare
