#include "LhaLh23Decoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <queue>
#include <stdexcept>

namespace {

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size)
        : data_(data), size_(size), bitOffset_(0) {}

    bool readBit(unsigned* value) {
        if (value == nullptr || remainingBits() == 0)
            return false;
        const size_t byteOffset = bitOffset_ >> 3;
        const unsigned shift = 7u - static_cast<unsigned>(bitOffset_ & 7u);
        *value = (data_[byteOffset] >> shift) & 1u;
        ++bitOffset_;
        return true;
    }

    bool readBits(unsigned count, uint32_t* value) {
        if (value == nullptr || count > 32 || count > remainingBits())
            return false;
        uint32_t result = 0;
        for (unsigned i = 0; i < count; ++i) {
            unsigned bit = 0;
            if (!readBit(&bit))
                return false;
            result = (result << 1) | bit;
        }
        *value = result;
        return true;
    }

private:
    size_t remainingBits() const {
        if (size_ > std::numeric_limits<size_t>::max() / 8)
            return std::numeric_limits<size_t>::max();
        const size_t total = size_ * 8;
        return bitOffset_ <= total ? total - bitOffset_ : 0;
    }

    const uint8_t* data_;
    size_t size_;
    size_t bitOffset_;
};

class History {
public:
    History() : writeIndex_(0) {
        bytes_.fill(static_cast<uint8_t>(' '));
    }

    void emit(uint8_t value, std::vector<uint8_t>* output) {
        output->push_back(value);
        bytes_[writeIndex_] = value;
        writeIndex_ = (writeIndex_ + 1) & (bytes_.size() - 1);
    }

    bool copy(size_t distance, size_t length, size_t outputSize,
              std::vector<uint8_t>* output) {
        if (output->size() > outputSize || distance == 0 || distance > bytes_.size() ||
            length > outputSize - output->size())
            return false;
        for (size_t i = 0; i < length; ++i) {
            const size_t source = (writeIndex_ + bytes_.size() - distance) &
                                  (bytes_.size() - 1);
            emit(bytes_[source], output);
        }
        return true;
    }

private:
    std::array<uint8_t, 8192> bytes_;
    size_t writeIndex_;
};

struct AdaptiveNode {
    int child[2];
    int parent;
    int symbol;
    uint32_t frequency;

    AdaptiveNode() : parent(-1), symbol(-1), frequency(0) {
        child[0] = child[1] = -1;
    }

    bool isLeaf() const { return child[0] < 0; }
};

class AdaptiveTree {
public:
    AdaptiveTree() : root_(-1), symbolCount_(0), presentLeaves_(0) {}

    bool initializeEqualLeaves(unsigned symbolCount) {
        if (symbolCount == 0)
            return false;
        nodes_.clear();
        order_.clear();
        leaves_.assign(symbolCount, -1);
        symbolCount_ = symbolCount;
        presentLeaves_ = symbolCount;

        std::vector<int> level;
        level.reserve(symbolCount);
        for (unsigned slot = 0; slot < symbolCount; ++slot) {
            AdaptiveNode leaf;
            leaf.symbol = static_cast<int>(symbolCount - slot - 1);
            leaf.frequency = 1;
            const int index = appendNode(leaf);
            leaves_[static_cast<size_t>(leaf.symbol)] = index;
            level.push_back(index);
        }
        while (level.size() > 1) {
            std::vector<int> next;
            next.reserve((level.size() + 1) / 2);
            for (size_t i = 0; i + 1 < level.size(); i += 2)
                next.push_back(appendInternal(level[i], level[i + 1]));
            if ((level.size() & 1u) != 0)
                next.push_back(level.back());
            level.swap(next);
        }
        root_ = level[0];
        rebuildOrder();
        return true;
    }

    bool initializePositionTree() {
        if (!initializeEqualLeaves(1))
            return false;
        nodes_[root_].symbol = 0;
        leaves_.assign(128, -1);
        leaves_[0] = root_;
        symbolCount_ = 128;
        presentLeaves_ = 1;
        rebuildOrder();
        return true;
    }

    int root() const { return root_; }

    int leaf(unsigned symbol) const {
        if (symbol >= leaves_.size())
            return -1;
        return leaves_[symbol];
    }

    uint32_t rootFrequency() const {
        return root_ >= 0 ? nodes_[root_].frequency : 0;
    }

    void setRootFrequencySentinel(uint32_t frequency) {
        if (root_ >= 0)
            nodes_[root_].frequency = frequency;
    }

    bool decode(BitReader* bits, unsigned* symbol) const {
        if (bits == nullptr || symbol == nullptr || root_ < 0)
            return false;
        int node = root_;
        while (!nodes_[node].isLeaf()) {
            unsigned bit = 0;
            if (!bits->readBit(&bit))
                return false;
            node = nodes_[node].child[bit];
            if (node < 0)
                return false;
        }
        if (nodes_[node].symbol < 0)
            return false;
        *symbol = static_cast<unsigned>(nodes_[node].symbol);
        return true;
    }

    bool update(unsigned symbol) {
        if (symbol >= leaves_.size() || leaves_[symbol] < 0)
            return false;
        int current = leaves_[symbol];
        while (current >= 0) {
            int leader = blockLeader(current);
            if (leader >= 0 && leader != current &&
                nodes_[leader].parent != current &&
                nodes_[current].parent != leader) {
                if (!swapPositions(current, leader))
                    return false;
                swapOrderEntries(current, leader);
            }
            if (nodes_[current].frequency == std::numeric_limits<uint32_t>::max())
                return false;
            ++nodes_[current].frequency;
            reorderNode(current);
            current = nodes_[current].parent;
        }
        return true;
    }

    bool addPositionSymbol(unsigned symbol) {
        if (symbol != presentLeaves_ || symbol >= symbolCount_)
            return false;
        int deepest = -1;
        unsigned deepestLevel = 0;
        findDeepestLeaf(root_, 0, &deepestLevel, &deepest);
        if (deepest < 0)
            return false;

        AdaptiveNode oldLeaf = nodes_[deepest];
        AdaptiveNode parent;
        parent.frequency = oldLeaf.frequency;
        const int parentIndex = appendNode(parent);
        const int newLeafIndex = appendNode(AdaptiveNode());
        nodes_[newLeafIndex].symbol = static_cast<int>(symbol);
        nodes_[newLeafIndex].parent = parentIndex;
        nodes_[newLeafIndex].frequency = 0;
        nodes_[parentIndex].child[0] = deepest;
        nodes_[parentIndex].child[1] = newLeafIndex;
        nodes_[parentIndex].parent = oldLeaf.parent;
        nodes_[deepest].parent = parentIndex;

        if (oldLeaf.parent < 0) {
            root_ = parentIndex;
        } else {
            const int side = nodes_[oldLeaf.parent].child[0] == deepest ? 0 : 1;
            nodes_[oldLeaf.parent].child[side] = parentIndex;
        }
        leaves_[symbol] = newLeafIndex;
        ++presentLeaves_;
        order_.push_back(newLeafIndex);
        order_.push_back(parentIndex);
        reorderNode(parentIndex);
        return true;
    }

    bool rebuildHalved() {
        if (symbolCount_ == 0)
            return false;
        std::vector<uint32_t> frequencies(presentLeaves_, 0);
        for (unsigned symbol = 0; symbol < presentLeaves_; ++symbol) {
            const int index = leaves_[symbol];
            if (index < 0)
                return false;
            frequencies[symbol] = (nodes_[index].frequency + 1) / 2;
        }

        struct QueueItem {
            uint32_t frequency;
            uint64_t sequence;
            int node;
        };
        struct Later {
            bool operator()(const QueueItem& left, const QueueItem& right) const {
                if (left.frequency != right.frequency)
                    return left.frequency > right.frequency;
                return left.sequence > right.sequence;
            }
        };
        std::priority_queue<QueueItem, std::vector<QueueItem>, Later> queue;
        nodes_.clear();
        order_.clear();
        leaves_.assign(symbolCount_, -1);
        uint64_t sequence = 0;
        for (unsigned slot = 0; slot < presentLeaves_; ++slot) {
            const unsigned symbol = presentLeaves_ - slot - 1;
            AdaptiveNode leafNode;
            leafNode.symbol = static_cast<int>(symbol);
            leafNode.frequency = frequencies[symbol];
            const int index = appendNode(leafNode);
            leaves_[symbol] = index;
            queue.push(QueueItem{leafNode.frequency, sequence++, index});
        }
        while (queue.size() > 1) {
            const QueueItem first = queue.top();
            queue.pop();
            const QueueItem second = queue.top();
            queue.pop();
            const int parent = appendInternal(first.node, second.node);
            queue.push(QueueItem{nodes_[parent].frequency, sequence++, parent});
        }
        root_ = queue.top().node;
        rebuildOrder();
        return true;
    }

private:
    int appendNode(const AdaptiveNode& node) {
        nodes_.push_back(node);
        return static_cast<int>(nodes_.size() - 1);
    }

    int appendInternal(int left, int right) {
        AdaptiveNode node;
        node.child[0] = left;
        node.child[1] = right;
        node.frequency = nodes_[left].frequency + nodes_[right].frequency;
        const int index = appendNode(node);
        nodes_[left].parent = index;
        nodes_[right].parent = index;
        return index;
    }

    void rebuildOrder() {
        order_.resize(nodes_.size());
        for (size_t i = 0; i < nodes_.size(); ++i)
            order_[i] = static_cast<int>(i);
        std::stable_sort(order_.begin(), order_.end(), [this](int left, int right) {
            return nodes_[left].frequency > nodes_[right].frequency;
        });
    }

    int blockLeader(int node) const {
        if (node < 0)
            return -1;
        const uint32_t frequency = nodes_[node].frequency;
        int leader = node;
        for (std::vector<int>::const_iterator it = order_.begin();
             it != order_.end(); ++it) {
            if (nodes_[*it].frequency == frequency)
                leader = *it;
        }
        return leader;
    }

    bool swapPositions(int first, int second) {
        const int firstParent = nodes_[first].parent;
        const int secondParent = nodes_[second].parent;
        if (firstParent == second || secondParent == first)
            return false;
        if (firstParent >= 0 && firstParent == secondParent) {
            const int firstSide = nodes_[firstParent].child[0] == first ? 0 : 1;
            const int secondSide = 1 - firstSide;
            std::swap(nodes_[firstParent].child[firstSide],
                      nodes_[firstParent].child[secondSide]);
            nodes_[first].parent = secondParent;
            nodes_[second].parent = firstParent;
            return true;
        }
        if (firstParent < 0)
            root_ = second;
        else {
            const int side = nodes_[firstParent].child[0] == first ? 0 : 1;
            nodes_[firstParent].child[side] = second;
        }
        if (secondParent < 0)
            root_ = first;
        else {
            const int side = nodes_[secondParent].child[0] == second ? 0 : 1;
            nodes_[secondParent].child[side] = first;
        }
        nodes_[first].parent = secondParent;
        nodes_[second].parent = firstParent;
        return true;
    }

    void swapOrderEntries(int first, int second) {
        std::vector<int>::iterator firstIt = std::find(order_.begin(), order_.end(), first);
        std::vector<int>::iterator secondIt = std::find(order_.begin(), order_.end(), second);
        if (firstIt != order_.end() && secondIt != order_.end())
            std::iter_swap(firstIt, secondIt);
    }

    void reorderNode(int node) {
        std::vector<int>::iterator it = std::find(order_.begin(), order_.end(), node);
        if (it == order_.end())
            return;
        order_.erase(it);
        const uint32_t frequency = nodes_[node].frequency;
        std::vector<int>::iterator position = order_.begin();
        while (position != order_.end() && nodes_[*position].frequency >= frequency)
            ++position;
        order_.insert(position, node);
    }

    void findDeepestLeaf(int node, unsigned depth, unsigned* deepestLevel,
                         int* deepest) const {
        if (node < 0)
            return;
        if (nodes_[node].isLeaf()) {
            if (*deepest < 0 || depth > *deepestLevel ||
                (depth == *deepestLevel && node > *deepest)) {
                *deepestLevel = depth;
                *deepest = node;
            }
            return;
        }
        findDeepestLeaf(nodes_[node].child[0], depth + 1, deepestLevel, deepest);
        findDeepestLeaf(nodes_[node].child[1], depth + 1, deepestLevel, deepest);
    }

    std::vector<AdaptiveNode> nodes_;
    std::vector<int> order_;
    std::vector<int> leaves_;
    int root_;
    unsigned symbolCount_;
    unsigned presentLeaves_;
};

class Lh2DynamicTree {
public:
    Lh2DynamicTree()
        : isPosition_(false), activeEnd_(0), latestPositionLeaf_(0),
          leafCount_(0), positionUpdateCount_(0), nextBlock_(2) {}

    bool initializeLiteralTree() {
        isPosition_ = false;
        activeEnd_ = 571;
        leafCount_ = 286;
        if (!initializeArrays(activeEnd_))
            return false;
        for (unsigned symbol = 0; symbol < 286; ++symbol) {
            const int node = 570 - static_cast<int>(symbol);
            nodes_[node].child = -1 - static_cast<int>(symbol);
            nodes_[node].frequency = 1;
            nodes_[node].block = 1;
            symbolNode_[symbol] = node;
        }
        blockEdge_[1] = 285;
        for (int node = 284, child = 570; node >= 0; --node, child -= 2) {
            nodes_[node].child = child;
            nodes_[node].frequency = nodes_[child].frequency +
                                     nodes_[child - 1].frequency;
            nodes_[child].parent = node;
            nodes_[child - 1].parent = node;
            if (nodes_[node].frequency == nodes_[node + 1].frequency) {
                nodes_[node].block = nodes_[node + 1].block;
                blockEdge_[nodes_[node].block] = node;
            } else if (!createBlock(node)) {
                return false;
            }
        }
        nodes_[0].parent = -1;
        return true;
    }

    bool initializePositionTree() {
        isPosition_ = true;
        activeEnd_ = 1;
        leafCount_ = 1;
        latestPositionLeaf_ = 0;
        positionUpdateCount_ = 0;
        if (!initializeArrays(256))
            return false;
        nodes_[0].child = -1;
        nodes_[0].frequency = 1;
        symbolNode_[0] = 0;
        return createBlock(0);
    }

    bool decode(BitReader* bits, unsigned* symbol) const {
        if (bits == nullptr || symbol == nullptr || nodes_.empty())
            return false;
        int node = 0;
        while (nodes_[node].child >= 0) {
            unsigned bit = 0;
            if (!bits->readBit(&bit))
                return false;
            node = nodes_[node].child - static_cast<int>(bit);
            if (node < 0 || static_cast<size_t>(node) >= activeEnd_)
                return false;
        }
        const int decoded = ~nodes_[node].child;
        if (decoded < 0 || static_cast<size_t>(decoded) >= symbolNode_.size())
            return false;
        *symbol = static_cast<unsigned>(decoded);
        return true;
    }

    bool update(unsigned symbol) {
        if (symbol >= symbolNode_.size() || symbolNode_[symbol] < 0)
            return false;
        if (isPosition_) {
            if (positionUpdateCount_ == 0x8000) {
                if (!reconstruct())
                    return false;
                positionUpdateCount_ = nodes_[0].frequency;
                nodes_[0].frequency = 0xffff;
            }
        } else {
            if (nodes_[0].frequency == 0x8000 && !reconstruct())
                return false;
            if (nodes_[0].frequency == std::numeric_limits<uint32_t>::max())
                return false;
            ++nodes_[0].frequency;
        }

        int node = symbolNode_[symbol];
        while (node != 0) {
            node = incrementBlockLeader(node);
            if (node < 0 || static_cast<size_t>(node) >= activeEnd_)
                return false;
        }
        if (isPosition_)
            ++positionUpdateCount_;
        return true;
    }

    bool addPositionSymbol(unsigned symbol) {
        if (!isPosition_ || symbol != leafCount_ || symbol >= 128)
            return false;
        const int parent = latestPositionLeaf_;
        const int oldLeaf = parent + 1;
        const int newLeaf = parent + 2;
        if (static_cast<size_t>(newLeaf) >= nodes_.size() ||
            nodes_[parent].child >= 0)
            return false;

        const int oldChild = nodes_[parent].child;
        const int oldBlock = nodes_[parent].block;
        nodes_[oldLeaf] = Lh2Node();
        nodes_[oldLeaf].child = oldChild;
        nodes_[oldLeaf].frequency = nodes_[parent].frequency;
        nodes_[oldLeaf].block = oldBlock;
        nodes_[oldLeaf].parent = parent;
        if (oldChild < 0)
            symbolNode_[static_cast<size_t>(~oldChild)] = oldLeaf;

        nodes_[newLeaf] = Lh2Node();
        nodes_[newLeaf].child = -1 - static_cast<int>(symbol);
        nodes_[newLeaf].parent = parent;
        nodes_[newLeaf].frequency = 0;
        nodes_[parent].child = newLeaf;
        if (parent == 0) {
            if (blockEdge_[oldBlock] != parent)
                return false;
            blockEdge_[oldBlock] = oldLeaf;
            nodes_[parent].frequency = 0xffff;
        }
        if (!createBlock(newLeaf))
            return false;
        symbolNode_[symbol] = newLeaf;
        latestPositionLeaf_ = newLeaf;
        activeEnd_ = static_cast<size_t>(newLeaf + 1);
        ++leafCount_;
        return true;
    }

private:
    struct Lh2Node {
        int child;
        int parent;
        int block;
        uint32_t frequency;

        Lh2Node() : child(-1), parent(-1), block(0), frequency(0) {}
    };

    bool initializeArrays(size_t nodeCount) {
        nodes_.assign(nodeCount, Lh2Node());
        symbolNode_.assign(286, -1);
        blockEdge_.assign(nodeCount * 2 + 4, -1);
        blockStack_.resize(blockEdge_.size());
        for (size_t i = 0; i < blockStack_.size(); ++i)
            blockStack_[i] = static_cast<int>(i);
        nextBlock_ = 2;
        return true;
    }

    bool createBlock(int node) {
        if (nextBlock_ >= blockStack_.size())
            return false;
        const int block = blockStack_[nextBlock_++];
        nodes_[node].block = block;
        blockEdge_[block] = node;
        return true;
    }

    void releaseBlock(int block) {
        if (block >= 2 && nextBlock_ > 2)
            blockStack_[--nextBlock_] = block;
    }

    bool exchangeContents(int first, int second) {
        const int firstChild = nodes_[first].child;
        const int secondChild = nodes_[second].child;
        nodes_[first].child = secondChild;
        nodes_[second].child = firstChild;
        if (firstChild >= 0) {
            if (firstChild <= 0 || static_cast<size_t>(firstChild) >= activeEnd_ ||
                static_cast<size_t>(firstChild - 1) >= activeEnd_)
                return false;
            nodes_[firstChild].parent = second;
            nodes_[firstChild - 1].parent = second;
        } else {
            symbolNode_[static_cast<size_t>(~firstChild)] = second;
        }
        if (secondChild >= 0) {
            if (secondChild <= 0 || static_cast<size_t>(secondChild) >= activeEnd_ ||
                static_cast<size_t>(secondChild - 1) >= activeEnd_)
                return false;
            nodes_[secondChild].parent = first;
            nodes_[secondChild - 1].parent = first;
        } else {
            symbolNode_[static_cast<size_t>(~secondChild)] = first;
        }
        return true;
    }

    int incrementBlockLeader(int node) {
        if (node < 0 || static_cast<size_t>(node) >= activeEnd_)
            return -1;
        const int block = nodes_[node].block;
        if (block < 0 || static_cast<size_t>(block) >= blockEdge_.size())
            return -1;
        int leader = blockEdge_[block];
        if (leader < 0 || static_cast<size_t>(leader) >= activeEnd_)
            return -1;
        if (leader != node) {
            if (!exchangeContents(node, leader))
                return -1;
            node = leader;
        }
        if (nodes_[node].frequency == std::numeric_limits<uint32_t>::max())
            return -1;

        if (node + 1 < static_cast<int>(activeEnd_) &&
            nodes_[node + 1].block == block) {
            blockEdge_[block] = node + 1;
            ++nodes_[node].frequency;
            if (node > 0 && nodes_[node].frequency == nodes_[node - 1].frequency) {
                nodes_[node].block = nodes_[node - 1].block;
            } else if (!createBlock(node)) {
                return -1;
            }
        } else {
            ++nodes_[node].frequency;
            if (node > 0 && nodes_[node].frequency == nodes_[node - 1].frequency) {
                releaseBlock(block);
                nodes_[node].block = nodes_[node - 1].block;
            }
        }
        return nodes_[node].parent;
    }

    bool reconstruct() {
        if (activeEnd_ == 0 || (activeEnd_ & 1u) == 0)
            return false;
        const int end = static_cast<int>(activeEnd_);
        std::vector<Lh2Node> leaves;
        leaves.reserve(leafCount_);
        for (int i = 0; i < end; ++i) {
            if (nodes_[i].child < 0) {
                Lh2Node leaf = nodes_[i];
                leaf.frequency = (leaf.frequency + 1) / 2;
                leaves.push_back(leaf);
            }
        }
        if (leaves.size() != leafCount_)
            return false;
        for (int i = 0; i < end; ++i)
            nodes_[i] = Lh2Node();
        for (size_t i = 0; i < leaves.size(); ++i)
            nodes_[i] = leaves[i];

        int leafCursor = static_cast<int>(leaves.size()) - 1;
        int writeIndex = end - 1;
        int siblingStart = end - 2;
        while (writeIndex >= 0) {
            while (writeIndex >= siblingStart) {
                if (leafCursor < 0)
                    return false;
                nodes_[writeIndex--] = leaves[static_cast<size_t>(leafCursor--)];
            }
            const uint32_t combined = nodes_[siblingStart].frequency +
                                      nodes_[siblingStart + 1].frequency;
            int insertion = 0;
            while (insertion < end && combined < nodes_[insertion].frequency)
                ++insertion;
            while (leafCursor >= insertion) {
                if (writeIndex < 0)
                    return false;
                nodes_[writeIndex--] = leaves[static_cast<size_t>(leafCursor--)];
            }
            if (writeIndex < 0 || ((siblingStart + 1) & 1) != 0)
                return false;
            nodes_[writeIndex].frequency = combined;
            nodes_[writeIndex].child = siblingStart + 1;
            --writeIndex;
            siblingStart -= 2;
        }
        if (leafCursor != -1 || siblingStart != -1)
            return false;
        return rebuildParentsAndBlocks(end);
    }

    bool rebuildParentsAndBlocks(int end) {
        std::fill(symbolNode_.begin(), symbolNode_.end(), -1);
        std::fill(blockEdge_.begin(), blockEdge_.end(), -1);
        for (size_t i = 0; i < blockStack_.size(); ++i)
            blockStack_[i] = static_cast<int>(i);
        nextBlock_ = 2;

        int previousBlock = -1;
        uint32_t previousFrequency = 0;
        for (int i = 0; i < end; ++i) {
            nodes_[i].parent = -1;
            if (nodes_[i].child >= 0) {
                const int child = nodes_[i].child;
                if ((child & 1) != 0 || child <= 0 || child >= end || child - 1 >= end)
                    return false;
                nodes_[child].parent = i;
                nodes_[child - 1].parent = i;
            } else {
                const int symbol = ~nodes_[i].child;
                if (symbol < 0 || static_cast<size_t>(symbol) >= symbolNode_.size())
                    return false;
                symbolNode_[symbol] = i;
            }
            if (i == 0 || nodes_[i].frequency != previousFrequency) {
                if (!createBlock(i))
                    return false;
                previousBlock = nodes_[i].block;
            } else {
                nodes_[i].block = previousBlock;
            }
            previousFrequency = nodes_[i].frequency;
        }
        nodes_[0].parent = -1;
        return true;
    }

    std::vector<Lh2Node> nodes_;
    std::vector<int> symbolNode_;
    std::vector<int> blockEdge_;
    std::vector<int> blockStack_;
    bool isPosition_;
    size_t activeEnd_;
    int latestPositionLeaf_;
    unsigned leafCount_;
    uint32_t positionUpdateCount_;
    size_t nextBlock_;
};

class CanonicalHuffman {
public:
    CanonicalHuffman() : singleSymbol_(-1), maximumLength_(0) {
        counts_.fill(0);
        firstCode_.fill(0);
        firstSymbol_.fill(0);
    }

    bool build(const std::vector<uint8_t>& lengths, unsigned maxBits,
               int singleSymbol = -1) {
        if (maxBits == 0 || maxBits > 16)
            return false;
        singleSymbol_ = singleSymbol;
        maximumLength_ = 0;
        counts_.fill(0);
        firstCode_.fill(0);
        firstSymbol_.fill(0);
        symbols_.clear();
        if (singleSymbol_ >= 0)
            return static_cast<size_t>(singleSymbol_) < lengths.size();

        for (size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            const unsigned length = lengths[symbol];
            if (length > maxBits)
                return false;
            if (length != 0) {
                ++counts_[length];
                maximumLength_ = std::max(maximumLength_, length);
            }
        }
        if (maximumLength_ == 0)
            return false;

        uint32_t code = 0;
        uint32_t kraft = 0;
        uint32_t symbolOffset = 0;
        for (unsigned length = 1; length <= maximumLength_; ++length) {
            code = (code + counts_[length - 1]) << 1;
            firstCode_[length] = code;
            firstSymbol_[length] = symbolOffset;
            symbolOffset += counts_[length];
            if (code + counts_[length] > (1u << length))
                return false;
            kraft += counts_[length] << (maximumLength_ - length);
        }
        if (kraft != (1u << maximumLength_))
            return false;
        for (unsigned length = 1; length <= maximumLength_; ++length) {
            for (size_t symbol = 0; symbol < lengths.size(); ++symbol) {
                if (lengths[symbol] == length)
                    symbols_.push_back(static_cast<uint16_t>(symbol));
            }
        }
        return true;
    }

    bool decode(BitReader* bits, unsigned* symbol) const {
        if (bits == nullptr || symbol == nullptr)
            return false;
        if (singleSymbol_ >= 0) {
            *symbol = static_cast<unsigned>(singleSymbol_);
            return true;
        }
        uint32_t code = 0;
        for (unsigned length = 1; length <= maximumLength_; ++length) {
            unsigned bit = 0;
            if (!bits->readBit(&bit))
                return false;
            code = (code << 1) | bit;
            if (code >= firstCode_[length] &&
                code - firstCode_[length] < counts_[length]) {
                const uint32_t index = firstSymbol_[length] + code - firstCode_[length];
                if (index >= symbols_.size())
                    return false;
                *symbol = symbols_[index];
                return true;
            }
        }
        return false;
    }

private:
    std::array<uint32_t, 17> counts_;
    std::array<uint32_t, 17> firstCode_;
    std::array<uint32_t, 17> firstSymbol_;
    std::vector<uint16_t> symbols_;
    int singleSymbol_;
    unsigned maximumLength_;
};

bool decodeLength(BitReader* bits, unsigned symbol, size_t* length) {
    if (length == nullptr || symbol < 256 || symbol > 285)
        return false;
    if (symbol <= 284) {
        *length = symbol - 253;
        return true;
    }
    uint32_t extra = 0;
    if (!bits->readBits(8, &extra))
        return false;
    *length = 32 + extra;
    return true;
}

bool emitMatchWithLength(BitReader* bits, unsigned positionGroup, size_t length,
                         History* history, size_t outputSize,
                         std::vector<uint8_t>* output) {
    uint32_t low = 0;
    if (positionGroup >= 128 || !bits->readBits(6, &low))
        return false;
    const size_t distance = static_cast<size_t>(positionGroup) * 64 + low + 1;
    return history->copy(distance, length, outputSize, output);
}

template <typename Tree>
bool decodeAdaptiveSymbol(BitReader* bits, const Tree* tree, unsigned* symbol) {
    return tree != nullptr && tree->decode(bits, symbol);
}

bool readLh3LengthTable(BitReader* bits, CanonicalHuffman* table) {
    std::vector<uint8_t> lengths(286, 0);
    for (unsigned symbol = 0; symbol < 286; ++symbol) {
        unsigned present = 0;
        if (!bits->readBit(&present))
            return false;
        if (present != 0) {
            uint32_t length = 0;
            if (!bits->readBits(4, &length))
                return false;
            lengths[symbol] = static_cast<uint8_t>(length + 1);
        }
        if (symbol == 2 && lengths[0] == 1 && lengths[1] == 1 && lengths[2] == 1) {
            uint32_t only = 0;
            if (!bits->readBits(9, &only) || only >= lengths.size())
                return false;
            return table->build(lengths, 16, static_cast<int>(only));
        }
    }
    return table->build(lengths, 16);
}

bool readLh3PositionTable(BitReader* bits, CanonicalHuffman* table) {
    unsigned present = 0;
    if (!bits->readBit(&present))
        return false;
    std::vector<uint8_t> lengths(128, 0);
    if (present == 0) {
        lengths[0] = 2;
        for (unsigned i = 1; i <= 2; ++i) {
            lengths[i] = 4;
        }
        for (unsigned i = 3; i <= 5; ++i) {
            lengths[i] = 5;
        }
        for (unsigned i = 6; i <= 12; ++i) {
            lengths[i] = 6;
        }
        for (unsigned i = 13; i <= 30; ++i) {
            lengths[i] = 7;
        }
        for (unsigned i = 31; i <= 77; ++i) {
            lengths[i] = 8;
        }
        for (unsigned i = 78; i <= 127; ++i) {
            lengths[i] = 9;
        }
        return table->build(lengths, 16);
    }

    for (unsigned symbol = 0; symbol < 128; ++symbol) {
        uint32_t length = 0;
        if (!bits->readBits(4, &length))
            return false;
        lengths[symbol] = static_cast<uint8_t>(length);
        if (symbol == 2 && lengths[0] == 1 && lengths[1] == 1 && lengths[2] == 1) {
            uint32_t only = 0;
            if (!bits->readBits(7, &only) || only >= lengths.size())
                return false;
            return table->build(lengths, 15, static_cast<int>(only));
        }
    }
    return table->build(lengths, 15);
}

bool decodeLh3(const uint8_t* input, size_t inputSize, size_t outputSize,
               std::vector<uint8_t>* output) {
    BitReader bits(input, inputSize);
    History history;
    while (output->size() < outputSize) {
        uint32_t commandCount = 0;
        if (!bits.readBits(16, &commandCount) || commandCount == 0)
            return false;
        CanonicalHuffman literalTable;
        CanonicalHuffman positionTable;
        if (!readLh3LengthTable(&bits, &literalTable) ||
            !readLh3PositionTable(&bits, &positionTable))
            return false;

        for (uint32_t command = 0; command < commandCount; ++command) {
            unsigned symbol = 0;
            if (!literalTable.decode(&bits, &symbol))
                return false;
            if (symbol < 256) {
                if (output->size() >= outputSize)
                    return false;
                history.emit(static_cast<uint8_t>(symbol), output);
                continue;
            }
            size_t length = 0;
            if (!decodeLength(&bits, symbol, &length) ||
                length > outputSize - output->size())
                return false;
            unsigned position = 0;
            if (!positionTable.decode(&bits, &position) ||
                !emitMatchWithLength(&bits, position, length, &history,
                                     outputSize, output))
                return false;
        }
    }
    return output->size() == outputSize;
}

bool decodeLh2(const uint8_t* input, size_t inputSize, size_t outputSize,
               std::vector<uint8_t>* output) {
    BitReader bits(input, inputSize);
    History history;
    Lh2DynamicTree literalTree;
    Lh2DynamicTree positionTree;
    if (!literalTree.initializeLiteralTree() ||
        !positionTree.initializePositionTree())
        return false;

    size_t nextPositionCount = 64;
    while (output->size() < outputSize) {
        uint32_t decoded = 0;
        if (!decodeAdaptiveSymbol(&bits, &literalTree, &decoded))
            return false;
        if (!literalTree.update(decoded))
            return false;

        if (decoded < 256) {
            if (output->size() >= outputSize)
                return false;
            history.emit(static_cast<uint8_t>(decoded), output);
            continue;
        }

        size_t length = 0;
        if (!decodeLength(&bits, decoded, &length) ||
            length > outputSize - output->size())
            return false;
        while (output->size() > nextPositionCount) {
            const unsigned group = static_cast<unsigned>(nextPositionCount / 64);
            if (!positionTree.addPositionSymbol(group) ||
                !positionTree.update(group))
                return false;
            nextPositionCount += 64;
        }

        unsigned position = 0;
        if (!decodeAdaptiveSymbol(&bits, &positionTree, &position) || position >= 128)
            return false;
        if (!positionTree.update(position))
            return false;
        uint32_t low = 0;
        if (!bits.readBits(6, &low))
            return false;
        const size_t distance = static_cast<size_t>(position) * 64 + low + 1;
        if (!history.copy(distance, length, outputSize, output))
            return false;
    }
    return output->size() == outputSize;
}

} // namespace

bool decompressLhaLh23(const char* method,
                       const uint8_t* input,
                       size_t inputSize,
                       size_t outputSize,
                       std::vector<uint8_t>* output) {
    if (method == nullptr || output == nullptr ||
        (input == nullptr && inputSize != 0) ||
        (std::strcmp(method, "-lh2-") != 0 && std::strcmp(method, "-lh3-") != 0))
        return false;
    try {
        output->clear();
        if (outputSize > output->max_size())
            return false;
        output->reserve(outputSize);
        if (outputSize == 0)
            return true;
        const bool result = std::strcmp(method, "-lh2-") == 0
                                ? decodeLh2(input, inputSize, outputSize, output)
                                : decodeLh3(input, inputSize, outputSize, output);
        if (!result)
            output->clear();
        return result;
    } catch (const std::bad_alloc&) {
        output->clear();
        return false;
    } catch (const std::length_error&) {
        output->clear();
        return false;
    }
}
