#include "AcronisRestoredStore.h"
#include "AcronisFatRestore.h"
#include "../modules/ModuleFormat.h"

#include <QByteArray>

#include <utility>

namespace peare {
namespace fs {

AcronisRestoredStore::AcronisRestoredStore(ByteStorePtr source, FilterLoader loadFilter,
                                           bool reconstructWithoutFilter)
    : source_(std::move(source)), loadFilter_(std::move(loadFilter)),
      reconstructWithoutFilter_(reconstructWithoutFilter) {}

std::int64_t AcronisRestoredStore::capacity() const {
    return source_ ? source_->capacity() : -1;
}

ByteStorePtr AcronisRestoredStore::resolveExternalWofResource(
    const std::string& path, const WofWimReference& reference) const {
    const auto resolver = std::dynamic_pointer_cast<const IWofResourceResolver>(source_);
    return resolver ? resolver->resolveExternalWofResource(path, reference) : nullptr;
}

bool AcronisRestoredStore::validate() const {
    return source_ && source_->validate();
}

void AcronisRestoredStore::initialize() const {
    try {
        if (!source_) return;
        if (source_->capacity() < 512) {
            view_ = source_;
            return;
        }
        QByteArray boot(512, '\0');
        if (!source_->readExactlyChecked(0, reinterpret_cast<std::uint8_t*>(boot.data()), boot.size())) return;
        if (ModuleFormatDetector::detectBuffer(boot).format != ModuleFormat::FAT) {
            view_ = source_;
            return;
        }
        ByteStorePtr filter;
        if (!loadFilter_ || !loadFilter_(&filter)) return;
        if (!filter || filter->capacity() == 0) {
            if (reconstructWithoutFilter_) {
                view_ = reconstructAcronisFat(source_, ByteStorePtr());
                return;
            }
            view_ = source_;
            return;
        }
        view_ = reconstructAcronisFat(source_, filter);
    } catch (...) {
        view_.reset();
    }
}

int AcronisRestoredStore::read(std::int64_t pos, std::uint8_t* dst, int count) const {
    std::call_once(once_, [this] { initialize(); });
    return view_ ? view_->read(pos, dst, count) : -1;
}

} // namespace fs
} // namespace peare
