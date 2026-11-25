#include "TibOldFileIndex.h"

#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace peare {
namespace fs {
namespace {

class IndexSink final : public TibOldFieldSink, private TibOldEnvelopeSink {
public:
    explicit IndexSink(const TibOldIndexLimits& limits)
        : limits_(limits), status_(TibOldIndexStatus::Ok),
          recordStatus_(TibOldCatalogStatus::Ok), current_(noRecord()),
          metadataBytes_(0), candidates_(0), sectionStart_(0), sectionOpen_(false),
          closedRecords_(0), closedSnapshots_(0), primaryLength_(0),
          envelopeStatus_(TibOldEnvelopeStatus::NeedMore) {}

    bool beginField(const TibOldField& field) override {
        if (status_ != TibOldIndexStatus::Ok) return false;
        tag_ = field.tag;
        data_.clear();
        envelope_.reset();
        primaryLength_ = 0;
        envelopeStatus_ = TibOldEnvelopeStatus::NeedMore;
        if (tag_ == 0x02 && current_ != noRecord() && index_.records[current_].metadata.type == 0) {
            try {
                envelope_.reset(new TibOldFileEnvelope(*this));
            } catch (const std::bad_alloc&) {
                status_ = TibOldIndexStatus::AllocationFailure;
                return false;
            }
        }
        return true;
    }

    bool fieldData(const std::uint8_t* data, std::size_t size) override {
        if (status_ != TibOldIndexStatus::Ok) return false;
        if (tag_ == 0x02) {
            if (!envelope_) return true;
            if (envelopeStatus_ == TibOldEnvelopeStatus::NeedMore ||
                envelopeStatus_ == TibOldEnvelopeStatus::Complete)
                envelopeStatus_ = envelope_->push(data, size);
            if (envelopeStatus_ != TibOldEnvelopeStatus::NeedMore &&
                envelopeStatus_ != TibOldEnvelopeStatus::Complete) {
                status_ = TibOldIndexStatus::EnvelopeError;
                return false;
            }
            return true;
        }
        if (tag_ != 0x66 && tag_ != 0x67) return true;
        try {
            if (data_.size() > limits_.catalog.maxInputBytes ||
                size > limits_.catalog.maxInputBytes - data_.size() ||
                metadataBytes_ > limits_.maxMetadataBytes ||
                data_.size() > limits_.maxMetadataBytes - metadataBytes_ ||
                size > limits_.maxMetadataBytes - metadataBytes_ - data_.size()) {
                status_ = TibOldIndexStatus::LimitExceeded;
                return false;
            }
            data_.insert(data_.end(), data, data + size);
            return true;
        } catch (const std::bad_alloc&) {
            status_ = TibOldIndexStatus::AllocationFailure;
        } catch (const std::length_error&) {
            status_ = TibOldIndexStatus::LimitExceeded;
        }
        return false;
    }

    void endField(const TibOldField& field, bool verified) override {
        if (!verified || status_ != TibOldIndexStatus::Ok) return;
        try {
            commit(field);
        } catch (const std::bad_alloc&) {
            status_ = TibOldIndexStatus::AllocationFailure;
        } catch (const std::length_error&) {
            status_ = TibOldIndexStatus::LimitExceeded;
        }
    }

    void skippedSectionTail(const TibOldSkippedRange& range) override {
        if (status_ != TibOldIndexStatus::Ok) return;
        try {
            if (index_.skippedRanges.size() >= limits_.maxSkippedRanges) {
                status_ = TibOldIndexStatus::LimitExceeded;
                return;
            }
            index_.skippedRanges.push_back(range);
        } catch (const std::bad_alloc&) {
            status_ = TibOldIndexStatus::AllocationFailure;
        } catch (const std::length_error&) {
            status_ = TibOldIndexStatus::LimitExceeded;
        }
    }

    TibOldIndexStatus status() const { return status_; }
    TibOldCatalogStatus recordStatus() const { return recordStatus_; }
    TibOldEnvelopeStatus envelopeStatus() const { return envelopeStatus_; }
    bool sectionOpen() const { return sectionOpen_; }
    bool hasClosedSnapshot() const { return closedSnapshots_ != 0; }
    bool hasSkippedRanges() const { return !index_.skippedRanges.empty(); }
    TibOldFileIndex take() {
        index_.trailingSectionIncomplete = sectionOpen_;
        return std::move(index_);
    }
    TibOldFileIndex takeClosed() {
        index_.records.resize(closedRecords_);
        index_.snapshots.resize(closedSnapshots_);
        index_.trailingSectionIncomplete = true;
        return std::move(index_);
    }

private:
    static std::size_t noRecord() { return std::numeric_limits<std::size_t>::max(); }
    bool primaryLength(std::uint64_t length) override { primaryLength_ = length; return true; }
    bool primaryData(const std::uint8_t*, std::size_t) override { return true; }

    void commit(const TibOldField& field) {
        sectionOpen_ = true;
        if (field.tag == 0x66) {
            current_ = noRecord();
            if (index_.records.size() >= limits_.maxRecords ||
                records_.size() >= limits_.catalog.maxRecords) {
                status_ = TibOldIndexStatus::LimitExceeded;
                return;
            }
            TibOldIndexRecord record;
            recordStatus_ = parseTibOldCatalogRecord(data_, field.start.logicalOffset,
                                                      limits_.catalog, &record.metadata);
            if (recordStatus_ != TibOldCatalogStatus::Ok) {
                status_ = recordStatus_ == TibOldCatalogStatus::LimitExceeded
                    ? TibOldIndexStatus::LimitExceeded : TibOldIndexStatus::CatalogRecordError;
                return;
            }
            if (offsets_.find(record.metadata.tagOffset) != offsets_.end()) {
                status_ = TibOldIndexStatus::DuplicateRecord;
                return;
            }
            record.field = field;
            records_.push_back(record.metadata);
            try {
                index_.records.push_back(std::move(record));
            } catch (...) {
                records_.pop_back();
                throw;
            }
            try {
                offsets_.emplace(records_.back().tagOffset, records_.size() - 1);
            } catch (...) {
                index_.records.pop_back();
                records_.pop_back();
                throw;
            }
            metadataBytes_ += data_.size();
            current_ = index_.records.size() - 1;
        } else if (field.tag == 0x02) {
            if (current_ == noRecord()) {
                status_ = TibOldIndexStatus::UnassociatedPayload;
            } else if (index_.records[current_].metadata.type == 1) {
                return;
            } else if (index_.records[current_].hasPayload) {
                status_ = TibOldIndexStatus::DuplicatePayload;
            } else {
                envelopeStatus_ = envelope_->finish();
                if (envelopeStatus_ != TibOldEnvelopeStatus::Complete) {
                    status_ = TibOldIndexStatus::EnvelopeError;
                    return;
                }
                index_.records[current_].payload = field;
                index_.records[current_].primaryLength = primaryLength_;
                index_.records[current_].hasPayload = true;
            }
        } else if (field.tag == 0x67) {
            current_ = noRecord();
            if (sectionStart_ < index_.snapshots.size()) {
                status_ = TibOldIndexStatus::DuplicateCatalog;
                return;
            }
            if (index_.snapshots.size() >= limits_.maxCatalogs) {
                status_ = TibOldIndexStatus::LimitExceeded;
                return;
            }
            TibOldIndexSnapshot snapshot;
            snapshot.field = field;
            snapshot.catalogStatus = scanTibOldCatalog(data_, records_, offsets_, limits_.catalog, &snapshot.scan);
            if (snapshot.catalogStatus == TibOldCatalogStatus::LimitExceeded) {
                status_ = TibOldIndexStatus::LimitExceeded;
                return;
            }
            if (snapshot.scan.candidates.size() > limits_.maxTotalCandidates - candidates_) {
                status_ = TibOldIndexStatus::LimitExceeded;
                return;
            }
            candidates_ += snapshot.scan.candidates.size();
            if (snapshot.catalogStatus == TibOldCatalogStatus::Ok)
                snapshot.catalogStatus = buildTibOldCatalogTree(snapshot.scan, records_, offsets_, limits_.catalog,
                                                               &snapshot.nodes);
            if (snapshot.catalogStatus == TibOldCatalogStatus::LimitExceeded) {
                status_ = TibOldIndexStatus::LimitExceeded;
                return;
            }
            metadataBytes_ += data_.size();
            index_.snapshots.push_back(std::move(snapshot));
        } else if (field.tag == 0x68) {
            current_ = noRecord();
            for (std::size_t i = sectionStart_; i < index_.snapshots.size(); ++i)
                index_.snapshots[i].sectionClosed = true;
            if (sectionStart_ < index_.snapshots.size()) {
                closedRecords_ = index_.records.size();
                closedSnapshots_ = index_.snapshots.size();
            }
            sectionStart_ = index_.snapshots.size();
            sectionOpen_ = false;
        }
    }

    const TibOldIndexLimits& limits_;
    TibOldIndexStatus status_;
    TibOldCatalogStatus recordStatus_;
    std::uint8_t tag_ = 0;
    std::size_t current_;
    std::size_t metadataBytes_;
    std::size_t candidates_;
    std::size_t sectionStart_;
    bool sectionOpen_;
    std::size_t closedRecords_;
    std::size_t closedSnapshots_;
    std::uint64_t primaryLength_;
    TibOldEnvelopeStatus envelopeStatus_;
    std::unique_ptr<TibOldFileEnvelope> envelope_;
    std::vector<std::uint8_t> data_;
    std::vector<TibOldCatalogRecord> records_;
    TibOldCatalogRecordIndex offsets_;
    TibOldFileIndex index_;
};

} // namespace

TibOldIndexStatus buildTibOldFileIndex(
    const std::vector<ByteStorePtr>& volumes, const TibOldIndexLimits& limits,
    TibOldFileIndex* output, TibOldFieldStatus* fieldStatus,
    TibOldCatalogStatus* recordStatus, TibOldEnvelopeStatus* envelopeStatus) {
    if (fieldStatus) *fieldStatus = TibOldFieldStatus::Ok;
    if (recordStatus) *recordStatus = TibOldCatalogStatus::Ok;
    if (envelopeStatus) *envelopeStatus = TibOldEnvelopeStatus::NeedMore;
    if (!output) return TibOldIndexStatus::InvalidArgument;
    try {
        IndexSink sink(limits);
        const TibOldFieldStatus scanned = scanTibOldFileFields(volumes, limits.fields, &sink);
        if (fieldStatus) *fieldStatus = scanned;
        if (recordStatus) *recordStatus = sink.recordStatus();
        if (envelopeStatus) *envelopeStatus = sink.envelopeStatus();
        if (sink.status() != TibOldIndexStatus::Ok) return sink.status();
        if (scanned == TibOldFieldStatus::TruncatedField ||
            scanned == TibOldFieldStatus::TruncatedChecksum ||
            ((scanned == TibOldFieldStatus::Ok || scanned == TibOldFieldStatus::OkWithSkippedData) &&
             sink.sectionOpen())) {
            if (sink.hasClosedSnapshot() || sink.hasSkippedRanges()) *output = sink.takeClosed();
            return TibOldIndexStatus::IncompleteTail;
        }
        if (scanned == TibOldFieldStatus::OkWithSkippedData) {
            *output = sink.take();
            return TibOldIndexStatus::UnverifiedData;
        }
        if (scanned != TibOldFieldStatus::Ok) return TibOldIndexStatus::FieldError;
        *output = sink.take();
        return TibOldIndexStatus::Ok;
    } catch (const std::bad_alloc&) {
        return TibOldIndexStatus::AllocationFailure;
    } catch (const std::length_error&) {
        return TibOldIndexStatus::LimitExceeded;
    }
}

} // namespace fs
} // namespace peare
