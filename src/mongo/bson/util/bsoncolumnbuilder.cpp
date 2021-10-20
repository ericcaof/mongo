/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/bson/util/bsoncolumnbuilder.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bsoncolumn_util.h"

#include "mongo/bson/util/simple8b_type_util.h"

#include <memory>

namespace mongo {
using namespace bsoncolumn;

namespace {
static constexpr uint8_t kMaxCount = 16;
static constexpr uint8_t kCountMask = 0x0F;
static constexpr uint8_t kControlMask = 0xF0;
static constexpr std::ptrdiff_t kNoSimple8bControl = -1;

static constexpr std::array<uint8_t, Simple8bTypeUtil::kMemoryAsInteger + 1>
    kControlByteForScaleIndex = {0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0x80};

// Encodes the double with the lowest possible scale index. In worst case we will interpret the
// memory as integer which is guaranteed to succeed.
std::pair<int64_t, uint8_t> scaleAndEncodeDouble(double value, uint8_t minScaleIndex) {
    boost::optional<int64_t> encoded;
    for (; !encoded; ++minScaleIndex) {
        encoded = Simple8bTypeUtil::encodeDouble(value, minScaleIndex);
    }

    // Subtract the last scale that was added in the loop before returning
    return {*encoded, minScaleIndex - 1};
}

// Checks if it is possible to do delta of ObjectIds
bool objectIdDeltaPossible(BSONElement elem, BSONElement prev) {
    return !memcmp(prev.OID().getInstanceUnique().bytes,
                   elem.OID().getInstanceUnique().bytes,
                   OID::kInstanceUniqueSize);
}

// Internal recursion function for traverseLockStep() when we just need to traverse reference
// object.
template <typename ElementFunc>
void _traverse(const BSONObj& reference, const ElementFunc& elemFunc) {
    for (const auto& elem : reference) {
        if (elem.type() == Object) {
            _traverse(elem.Obj(), elemFunc);
        } else {
            elemFunc(elem, BSONElement());
        }
    }
}

// Internal recursion function for traverseLockStep(). See documentation for traverseLockStep.
template <typename ElementFunc>
std::pair<BSONObj::iterator, bool> _traverseLockStep(const BSONObj& reference,
                                                     const BSONObj& obj,
                                                     const ElementFunc& elemFunc) {
    auto it = obj.begin();
    auto end = obj.end();
    for (const auto& elem : reference) {
        if (elem.type() == Object) {
            BSONObj refObj = elem.Obj();
            bool hasIt = it != end;
            // If refObj is empty, there must also exist an empty object on 'it' for this to be
            // valid. First we check that we have something on 'it'
            if (!hasIt && refObj.isEmpty()) {
                return {it, false};
            }

            bool elemMatch = hasIt && elem.fieldNameStringData() == it->fieldNameStringData();
            if (elemMatch) {
                // If 'reference' element is Object then 'obj' must also be Object.
                if (it->type() != Object) {
                    return {it, false};
                }

                // Differences in empty objects are not allowed.
                if (refObj.isEmpty() != it->Obj().isEmpty()) {
                    return {it, false};
                }

                // Everything match, recurse deeper.
                auto [_, compatible] = _traverseLockStep(refObj, (it++)->Obj(), elemFunc);
                if (!compatible) {
                    return {it, false};
                }
            } else {
                // Assume field name at 'it' is coming later in 'reference'. Traverse as if it is
                // missing from 'obj'. We don't increment the iterator in this case. If it is a
                // mismatch we will detect that at end when 'it' is not at 'end'.
                // Nothing can fail below this so traverse without all the checks.
                _traverse(refObj, elemFunc);
            }
        } else {
            // Non-object, call provided function with the two elements
            elemFunc(elem,
                     it != end && elem.fieldNameStringData() == it->fieldNameStringData()
                         ? *(it++)
                         : BSONElement());
        }
    }
    // Extra elements in 'obj' are not allowed. These needs to be merged in to 'reference' to be
    // able to compress.
    return {it, it == end};
}

// Traverses and validates BSONObj's in reference and obj in lock-step. Returns true if the object
// hierarchies are compatible for sub-object compression. To be compatible fields in 'obj' must be
// in the same order as in 'reference' and sub-objects in 'reference' must be sub-objects in 'obj'.
// The only difference between the two objects that is allowed is missing fields in 'obj' compared
// to 'reference'. 'ElementFunc' is called for every matching pair of BSONElement. Function
// signature should be void(const BSONElement&, const BSONElement&).
template <typename ElementFunc>
bool traverseLockStep(const BSONObj& reference, const BSONObj& obj, ElementFunc elemFunc) {
    auto [it, hierachyMatch] = _traverseLockStep(reference, obj, elemFunc);
    // Extra elements in 'obj' are not allowed. These needs to be merged in to 'reference' to be
    // able to compress.
    return hierachyMatch && it == obj.end();
}

// Internal recursion function for mergeObj(). See documentation for mergeObj. Returns true if merge
// was successful.
bool _mergeObj(BSONObjBuilder* builder, const BSONObj& reference, const BSONObj& obj) {
    auto refIt = reference.begin();
    auto refEnd = reference.end();
    auto it = obj.begin();
    auto end = obj.end();

    // Iterate until we reach end of any of the two objects.
    while (refIt != refEnd && it != end) {
        StringData name = refIt->fieldNameStringData();
        if (name == it->fieldNameStringData()) {
            bool refIsObj = refIt->type() == Object;
            bool itIsObj = it->type() == Object;

            if (refIsObj && itIsObj) {
                BSONObj refObj = refIt->Obj();
                BSONObj itObj = it->Obj();
                // There may not be a mismatch in empty objects
                if (refObj.isEmpty() != itObj.isEmpty())
                    return false;

                // Recurse deeper
                BSONObjBuilder subBuilder = builder->subobjStart(name);
                bool res = _mergeObj(&subBuilder, refObj, itObj);
                if (!res) {
                    return false;
                }
            } else if (refIsObj || itIsObj) {
                // Both or neither elements must be Object to be mergable
                return false;
            } else {
                // If name match and neither is Object we can append from reference and increment
                // both objects.
                builder->append(*refIt);
            }

            ++refIt;
            ++it;
            continue;
        }

        // Name mismatch, first search in 'obj' if reference element exist later.
        auto n = std::next(it);
        auto namePos = std::find_if(
            n, end, [&name](const auto& elem) { return elem.fieldNameStringData() == name; });
        if (namePos == end) {
            // Reference element does not exist in 'obj' so add it and continue merging with just
            // this iterator incremented.
            builder->append(*(refIt++));
        } else {
            // Reference element do exist later in 'obj'. Add element in 'it' if it is the first
            // time we see it, fail otherwise (incompatible ordering).
            if (builder->hasField(it->fieldNameStringData())) {
                return false;
            }
            builder->append(*(it++));
        }
    }

    // Add remaining reference elements when we reached end in 'obj'.
    for (; refIt != refEnd; ++refIt) {
        // We cannot allow empty object mismatch
        if (refIt->type() == Object && refIt->Obj().isEmpty()) {
            return false;
        }
        if (builder->hasField(refIt->fieldNameStringData())) {
            return false;
        }
        builder->append(*refIt);
    }

    // Add remaining 'obj' elements when we reached end in 'reference'.
    for (; it != end; ++it) {
        // We cannot allow empty object mismatch
        if (it->type() == Object && it->Obj().isEmpty()) {
            return false;
        }

        if (builder->hasField(it->fieldNameStringData())) {
            return false;
        }
        builder->append(*it);
    }

    return true;
}

// Tries to merge in elements from 'obj' into 'reference'. For successful merge the elements that
// already exist in 'reference' must be in 'obj' in the same order. The merged object is returned in
// case of a successful merge, empty BSONObj is returned for failure. This is quite an expensive
// operation as we are merging unsorted objects. Time complexity is O(N^2).
BSONObj mergeObj(const BSONObj& reference, const BSONObj& obj) {
    BSONObjBuilder builder;
    if (!_mergeObj(&builder, reference, obj)) {
        builder.abandon();
        return BSONObj();
    }

    return builder.obj();
}

}  // namespace

BSONColumnBuilder::BSONColumnBuilder(StringData fieldName)
    : BSONColumnBuilder(fieldName, BufBuilder()) {}

BSONColumnBuilder::BSONColumnBuilder(StringData fieldName, BufBuilder&& builder)
    : _state(&_bufBuilder, nullptr), _bufBuilder(std::move(builder)), _fieldName(fieldName) {
    // Leave space for element count at the beginning
    static_assert(sizeof(_elementCount) == kElementCountBytes,
                  "Element count for BSONColumn should be 4 bytes");
    _bufBuilder.reset();
    _bufBuilder.skip(kElementCountBytes);
}

BSONColumnBuilder& BSONColumnBuilder::append(BSONElement elem) {
    auto type = elem.type();
    uassert(ErrorCodes::InvalidBSONType,
            "MinKey or MaxKey is not valid for storage",
            type != MinKey && type != MaxKey);

    if (type != Object || elem.Obj().isEmpty()) {
        // Flush previous sub-object compression when non-object is appended
        if (_mode != Mode::kRegular) {
            _flushSubObjMode();
        }
        _state.append(elem);
        ++_elementCount;
        return *this;
    }


    if (_mode == Mode::kRegular) {
        _startDetermineSubObjReference(elem.Obj());
        ++_elementCount;
        return *this;
    }

    if (_mode == Mode::kSubObjDeterminingReference) {
        auto obj = elem.Obj();

        // We are in DeterminingReference mode, check if this current object is compatible and merge
        // in any new fields that are discovered.
        uint32_t numElements = 0;
        if (!traverseLockStep(
                _referenceSubObj,
                obj,
                [this, &numElements](const BSONElement& ref, const BSONElement& elem) {
                    ++numElements;
                    uassert(ErrorCodes::InvalidBSONType,
                            "MinKey or MaxKey is not valid for storage",
                            elem.type() != MinKey && elem.type() != MaxKey);
                })) {
            BSONObj merged = mergeObj(_referenceSubObj, obj);
            if (merged.isEmptyPrototype()) {
                // If merge failed, flush current sub-object compression and start over.
                _flushSubObjMode();

                _referenceSubObj = obj.getOwned();
                _bufferedObjElements.push_back(_referenceSubObj);
                _mode = Mode::kSubObjDeterminingReference;
                ++_elementCount;
                return *this;
            }
            _referenceSubObj = merged;
        }

        // If we've buffered twice as many objects as we have sub-elements we will achieve good
        // compression so use the currently built reference.
        if (numElements * 2 >= _bufferedObjElements.size()) {
            _bufferedObjElements.push_back(obj.getOwned());
            ++_elementCount;
            return *this;
        }

        _finishDetermineSubObjReference();
    }

    // Reference already determined for sub-object compression, try to add this new object.
    _appendSubElements(elem.Obj());
    ++_elementCount;
    return *this;
}


BSONColumnBuilder& BSONColumnBuilder::skip() {
    ++_elementCount;
    if (_mode == Mode::kRegular) {
        _state.skip();
    } else if (_mode == Mode::kSubObjDeterminingReference) {
        _bufferedObjElements.push_back(BSONObj());
    } else {
        for (auto&& state : _subobjStates) {
            state.skip();
        }
    }

    return *this;
}

BSONBinData BSONColumnBuilder::finalize() {
    if (_mode == Mode::kRegular) {
        _state.flush();
    } else {
        _flushSubObjMode();
    }

    // Write EOO at the end
    _bufBuilder.appendChar(EOO);

    // Write element count at the beginning
    DataView(_bufBuilder.buf()).write<LittleEndian<uint32_t>>(_elementCount);

    return {_bufBuilder.buf(), _bufBuilder.len(), BinDataType::Column};
}

BufBuilder BSONColumnBuilder::detach() {
    return std::move(_bufBuilder);
}

BSONColumnBuilder::EncodingState::EncodingState(
    BufBuilder* bufBuilder, std::function<void(const char*, size_t)> controlBlockWriter)
    : _simple8bBuilder64(_createBufferWriter()),
      _simple8bBuilder128(_createBufferWriter()),
      _controlByteOffset(kNoSimple8bControl),
      _scaleIndex(Simple8bTypeUtil::kMemoryAsInteger),
      _bufBuilder(bufBuilder),
      _controlBlockWriter(controlBlockWriter) {
    // Store EOO type with empty field name as previous.
    _storePrevious(BSONElement());
}

BSONColumnBuilder::EncodingState::EncodingState(EncodingState&& other)
    : _prev(std::move(other._prev)),
      _prevSize(std::move(other._prevSize)),
      _prevCapacity(std::move(other._prevCapacity)),
      _prevDelta(std::move(other._prevDelta)),
      _simple8bBuilder64(_createBufferWriter()),
      _simple8bBuilder128(_createBufferWriter()),
      _storeWith128(std::move(other._storeWith128)),
      _controlByteOffset(std::move(other._controlByteOffset)),
      _prevEncoded64(std::move(other._prevEncoded64)),
      _prevEncoded128(std::move(other._prevEncoded128)),
      _lastValueInPrevBlock(std::move(other._lastValueInPrevBlock)),
      _scaleIndex(std::move(other._scaleIndex)),
      _bufBuilder(std::move(other._bufBuilder)),
      _controlBlockWriter(std::move(other._controlBlockWriter)) {}

BSONColumnBuilder::EncodingState& BSONColumnBuilder::EncodingState::operator=(EncodingState&& rhs) {
    _prev = std::move(rhs._prev);
    _prevSize = std::move(rhs._prevSize);
    _prevCapacity = std::move(rhs._prevCapacity);
    _prevDelta = std::move(rhs._prevDelta);
    _storeWith128 = std::move(rhs._storeWith128);
    _controlByteOffset = std::move(rhs._controlByteOffset);
    _prevEncoded64 = std::move(rhs._prevEncoded64);
    _prevEncoded128 = std::move(rhs._prevEncoded128);
    _lastValueInPrevBlock = std::move(rhs._lastValueInPrevBlock);
    _scaleIndex = std::move(rhs._scaleIndex);
    _bufBuilder = std::move(rhs._bufBuilder);
    _controlBlockWriter = std::move(rhs._controlBlockWriter);
    return *this;
}

void BSONColumnBuilder::EncodingState::append(BSONElement elem) {
    auto type = elem.type();
    auto previous = _previous();

    // If we detect a type change (or this is first value). Flush all pending values in Simple-8b
    // and write uncompressed literal. Reset all default values.
    if (previous.type() != elem.type()) {
        _storePrevious(elem);
        _simple8bBuilder128.flush();
        _simple8bBuilder64.flush();
        _writeLiteralFromPrevious();
        return;
    }

    // Store delta in Simple-8b if types match
    bool compressed = !usesDeltaOfDelta(type) && elem.binaryEqualValues(previous);
    if (compressed) {
        if (_storeWith128) {
            _simple8bBuilder128.append(0);
        } else {
            _simple8bBuilder64.append(0);
        }
    }

    if (!compressed) {
        if (_storeWith128) {
            auto appendEncoded = [&](int128_t encoded) {
                compressed = _simple8bBuilder128.append(
                    Simple8bTypeUtil::encodeInt128(calcDelta(encoded, _prevEncoded128)));
                _prevEncoded128 = encoded;
            };

            switch (type) {
                case String:
                    if (auto encoded = Simple8bTypeUtil::encodeString(elem.valueStringData())) {
                        appendEncoded(*encoded);
                    }
                    break;
                case BinData: {
                    int size;
                    const char* binary = elem.binData(size);
                    // We only do delta encoding of binary if the binary size is exactly the same.
                    // To support size difference we'd need to add a count to be able to reconstruct
                    // binaries starting with zero bytes. We don't want to waste bits for this.
                    if (size != previous.valuestrsize())
                        break;

                    if (auto encoded = Simple8bTypeUtil::encodeBinary(binary, size)) {
                        appendEncoded(*encoded);
                    }
                } break;
                case NumberDecimal:
                    appendEncoded(Simple8bTypeUtil::encodeDecimal128(elem._numberDecimal()));
                    break;
                default:
                    MONGO_UNREACHABLE;
            };
        } else if (type == NumberDouble) {
            compressed = _appendDouble(elem._numberDouble(), previous._numberDouble());
        } else {
            // Variable to indicate that it was possible to encode this BSONElement as an integer
            // for storage inside Simple8b. If encoding is not possible the element is stored as
            // uncompressed.
            bool encodingPossible = true;
            // Value to store in Simple8b if encoding is possible.
            int64_t value = 0;
            switch (type) {
                case NumberInt:
                    value = calcDelta(elem._numberInt(), previous._numberInt());
                    break;
                case NumberLong:
                    value = calcDelta(elem._numberLong(), previous._numberLong());
                    break;
                case jstOID: {
                    encodingPossible = objectIdDeltaPossible(elem, previous);
                    if (!encodingPossible)
                        break;

                    int64_t curEncoded = Simple8bTypeUtil::encodeObjectId(elem.OID());
                    value = calcDelta(curEncoded, _prevEncoded64);
                    _prevEncoded64 = curEncoded;
                    break;
                }
                case bsonTimestamp: {
                    int64_t currTimestampDelta =
                        calcDelta(elem.timestamp().asULL(), previous.timestamp().asULL());
                    value = calcDelta(currTimestampDelta, _prevDelta);
                    _prevDelta = currTimestampDelta;
                    break;
                }
                case Date:
                    value = calcDelta(elem.date().toMillisSinceEpoch(),
                                      previous.date().toMillisSinceEpoch());
                    break;
                case Bool:
                    value = calcDelta(elem.boolean(), previous.boolean());
                    break;
                case Undefined:
                case jstNULL:
                    value = 0;
                    break;
                case RegEx:
                case DBRef:
                case CodeWScope:
                case Symbol:
                case Object:
                case Array:
                    encodingPossible = false;
                    break;
                default:
                    MONGO_UNREACHABLE;
            };
            if (encodingPossible) {
                compressed = _simple8bBuilder64.append(Simple8bTypeUtil::encodeInt64(value));
            }
        }
    }
    _storePrevious(elem);

    // Store uncompressed literal if value is outside of range of encodable values.
    if (!compressed) {
        _simple8bBuilder128.flush();
        _simple8bBuilder64.flush();
        _writeLiteralFromPrevious();
    }
}

void BSONColumnBuilder::EncodingState::skip() {
    auto before = _bufBuilder->len();
    if (_storeWith128) {
        _simple8bBuilder128.skip();
    } else {
        _simple8bBuilder64.skip();
    }
    // Rescale previous known value if this skip caused Simple-8b blocks to be written
    if (before != _bufBuilder->len() && _previous().type() == NumberDouble) {
        std::tie(_prevEncoded64, _scaleIndex) = scaleAndEncodeDouble(_lastValueInPrevBlock, 0);
    }
}

void BSONColumnBuilder::EncodingState::flush() {
    _simple8bBuilder128.flush();
    _simple8bBuilder64.flush();

    if (_controlByteOffset != kNoSimple8bControl && _controlBlockWriter) {
        _controlBlockWriter(_bufBuilder->buf() + _controlByteOffset,
                            _bufBuilder->len() - _controlByteOffset);
    }
}

boost::optional<Simple8bBuilder<uint64_t>> BSONColumnBuilder::EncodingState::_tryRescalePending(
    int64_t encoded, uint8_t newScaleIndex) {
    // Encode last value in the previous block with old and new scale index. We know that scaling
    // with the old index is possible.
    int64_t prev = *Simple8bTypeUtil::encodeDouble(_lastValueInPrevBlock, _scaleIndex);
    boost::optional<int64_t> prevRescaled =
        Simple8bTypeUtil::encodeDouble(_lastValueInPrevBlock, newScaleIndex);

    // Fail if we could not rescale
    bool possible = prevRescaled.has_value();
    if (!possible)
        return boost::none;

    // Create a new Simple8bBuilder for the rescaled values. If any Simple8b block is finalized when
    // adding the new values then rescaling is less optimal than flushing with the current scale. So
    // we just record if this happens in our write callback.
    Simple8bBuilder<uint64_t> builder([&possible](uint64_t block) { possible = false; });

    // Iterate over our pending values, decode them back into double, rescale and append to our new
    // Simple8b builder
    for (const auto& pending : _simple8bBuilder64) {
        if (!pending) {
            builder.skip();
            continue;
        }

        // Apply delta to previous, decode to double and rescale
        prev = expandDelta(prev, Simple8bTypeUtil::decodeInt64(*pending));
        auto rescaled = Simple8bTypeUtil::encodeDouble(
            Simple8bTypeUtil::decodeDouble(prev, _scaleIndex), newScaleIndex);

        // Fail if we could not rescale
        if (!rescaled || !prevRescaled)
            return boost::none;

        // Append the scaled delta
        auto appended =
            builder.append(Simple8bTypeUtil::encodeInt64(calcDelta(*rescaled, *prevRescaled)));

        // Fail if are out of range for Simple8b or a block was written
        if (!appended || !possible)
            return boost::none;

        // Remember previous for next value
        prevRescaled = rescaled;
    }

    // Last add our new value
    auto appended =
        builder.append(Simple8bTypeUtil::encodeInt64(calcDelta(encoded, *prevRescaled)));
    if (!appended || !possible)
        return boost::none;

    // We managed to add all re-scaled values, this will thus compress better. Set write callback to
    // our buffer writer and return
    builder.setWriteCallback(_createBufferWriter());
    return builder;
}

bool BSONColumnBuilder::EncodingState::_appendDouble(double value, double previous) {
    // Scale with lowest possible scale index
    auto [encoded, scaleIndex] = scaleAndEncodeDouble(value, _scaleIndex);

    if (scaleIndex != _scaleIndex) {
        // New value need higher scale index. We have two choices:
        // (1) Re-scale pending values to use this larger scale factor
        // (2) Flush pending and start a new block with this higher scale factor
        // We try both options and select the one that compresses best
        auto rescaled = _tryRescalePending(encoded, scaleIndex);
        if (rescaled) {
            // Re-scale possible, use this Simple8b builder
            std::swap(_simple8bBuilder64, *rescaled);
            _prevEncoded64 = encoded;
            _scaleIndex = scaleIndex;
            return true;
        }

        // Re-scale not possible, flush and start new block with the higher scale factor
        _simple8bBuilder64.flush();
        _controlByteOffset = kNoSimple8bControl;

        // Make sure value and previous are using the same scale factor.
        uint8_t prevScaleIndex;
        std::tie(_prevEncoded64, prevScaleIndex) = scaleAndEncodeDouble(previous, scaleIndex);
        if (scaleIndex != prevScaleIndex) {
            std::tie(encoded, scaleIndex) = scaleAndEncodeDouble(value, prevScaleIndex);
            std::tie(_prevEncoded64, prevScaleIndex) = scaleAndEncodeDouble(previous, scaleIndex);
        }

        // Record our new scale factor
        _scaleIndex = scaleIndex;
    }

    // Append delta and check if we wrote a Simple8b block. If we did we may be able to reduce the
    // scale factor when starting a new block
    auto before = _bufBuilder->len();
    if (!_simple8bBuilder64.append(
            Simple8bTypeUtil::encodeInt64(calcDelta(encoded, _prevEncoded64))))
        return false;

    if (_bufBuilder->len() != before) {
        // Reset the scale factor to 0 and append all pending values to a new Simple8bBuilder. In
        // the worse case we will end up with an identical scale factor.
        auto prevScale = _scaleIndex;
        std::tie(_prevEncoded64, _scaleIndex) = scaleAndEncodeDouble(_lastValueInPrevBlock, 0);

        // Create a new Simple8bBuilder.
        Simple8bBuilder<uint64_t> builder(_createBufferWriter());
        std::swap(_simple8bBuilder64, builder);

        // Iterate over previous pending values and re-add them recursively. That will increase the
        // scale factor as needed.
        auto prev = _lastValueInPrevBlock;
        auto prevEncoded = *Simple8bTypeUtil::encodeDouble(prev, prevScale);
        for (const auto& pending : builder) {
            if (pending) {
                prevEncoded = expandDelta(prevEncoded, Simple8bTypeUtil::decodeInt64(*pending));
                auto val = Simple8bTypeUtil::decodeDouble(prevEncoded, prevScale);
                _appendDouble(val, prev);
                prev = val;
            } else {
                _simple8bBuilder64.skip();
            }
        }
    }

    _prevEncoded64 = encoded;
    return true;
}

BSONElement BSONColumnBuilder::EncodingState::_previous() const {
    return {_prev.get(), 1, _prevSize, BSONElement::CachedSizeTag{}};
}


void BSONColumnBuilder::EncodingState::_storePrevious(BSONElement elem) {
    auto valuesize = elem.valuesize();

    // Add space for type byte and field name null terminator
    auto size = valuesize + 2;

    // Re-allocate buffer if not large enough
    if (size > _prevCapacity) {
        _prevCapacity = size;
        _prev = std::make_unique<char[]>(_prevCapacity);

        // Store null terminator, this byte will never change
        _prev[1] = '\0';
    }

    // Copy element into buffer for previous. Omit field name.
    _prev[0] = elem.type();
    memcpy(_prev.get() + 2, elem.value(), valuesize);
    _prevSize = size;
}

void BSONColumnBuilder::EncodingState::_writeLiteralFromPrevious() {
    // Write literal without field name and reset control byte to force new one to be written when
    // appending next value.
    if (_controlByteOffset != kNoSimple8bControl && _controlBlockWriter) {
        _controlBlockWriter(_bufBuilder->buf() + _controlByteOffset,
                            _bufBuilder->len() - _controlByteOffset);
    }
    _bufBuilder->appendBuf(_prev.get(), _prevSize);
    if (_controlBlockWriter) {
        _controlBlockWriter(_bufBuilder->buf() + _bufBuilder->len() - _prevSize, _prevSize);
    }


    // Reset state
    _controlByteOffset = kNoSimple8bControl;
    _scaleIndex = Simple8bTypeUtil::kMemoryAsInteger;
    _prevDelta = 0;

    _initializeFromPrevious();
}

void BSONColumnBuilder::EncodingState::_initializeFromPrevious() {
    // Initialize previous encoded when needed
    auto prevElem = _previous();
    auto type = prevElem.type();
    _storeWith128 = uses128bit(type);
    switch (type) {
        case NumberDouble:
            _lastValueInPrevBlock = prevElem._numberDouble();
            std::tie(_prevEncoded64, _scaleIndex) = scaleAndEncodeDouble(_lastValueInPrevBlock, 0);
            break;
        case String:
            _prevEncoded128 =
                Simple8bTypeUtil::encodeString(prevElem.valueStringData()).value_or(0);
            break;
        case BinData: {
            int size;
            const char* binary = prevElem.binData(size);
            _prevEncoded128 = Simple8bTypeUtil::encodeBinary(binary, size).value_or(0);
        } break;
        case NumberDecimal:
            _prevEncoded128 = Simple8bTypeUtil::encodeDecimal128(prevElem._numberDecimal());
            break;
        case jstOID:
            _prevEncoded64 = Simple8bTypeUtil::encodeObjectId(prevElem.__oid());
            break;
        default:
            break;
    }
}

ptrdiff_t BSONColumnBuilder::EncodingState::_incrementSimple8bCount() {
    char* byte;
    uint8_t count;
    uint8_t control = kControlByteForScaleIndex[_scaleIndex];

    if (_controlByteOffset == kNoSimple8bControl) {
        // Allocate new control byte if we don't already have one. Record its offset so we can find
        // it even if the underlying buffer reallocates.
        byte = _bufBuilder->skip(1);
        _controlByteOffset = std::distance(_bufBuilder->buf(), byte);
        count = 0;
    } else {
        // Read current count from previous control byte
        byte = _bufBuilder->buf() + _controlByteOffset;

        // If previous byte was written with a different control byte then we can't re-use and need
        // to start a new one
        if ((*byte & kControlMask) != control) {
            if (_controlBlockWriter) {
                _controlBlockWriter(_bufBuilder->buf() + _controlByteOffset,
                                    _bufBuilder->len() - _controlByteOffset);
            }
            _controlByteOffset = kNoSimple8bControl;
            _incrementSimple8bCount();
            return kNoSimple8bControl;
        }
        count = (*byte & kCountMask) + 1;
    }

    // Write back new count and clear offset if we have reached max count
    *byte = control | (count & kCountMask);
    if (count + 1 == kMaxCount) {
        auto prevControlByteOffset = _controlByteOffset;
        _controlByteOffset = kNoSimple8bControl;
        return prevControlByteOffset;
    }

    return kNoSimple8bControl;
}

Simple8bWriteFn BSONColumnBuilder::EncodingState::_createBufferWriter() {
    return [this](uint64_t block) {
        // Write/update block count
        ptrdiff_t fullControlOffset = _incrementSimple8bCount();

        // Write Simple-8b block in little endian byte order
        _bufBuilder->appendNum(block);

        // Write control block if this Simple-8b block made it full.
        if (_controlBlockWriter && fullControlOffset != kNoSimple8bControl) {
            _controlBlockWriter(_bufBuilder->buf() + fullControlOffset,
                                _bufBuilder->len() - fullControlOffset);
        }

        auto previous = _previous();
        if (previous.type() == NumberDouble) {
            _lastValueInPrevBlock = previous._numberDouble();
        }

        return true;
    };
}

void BSONColumnBuilder::_appendSubElements(const BSONObj& obj) {
    // Check if added object is compatible with selected reference object. Collect a flat vector of
    // all elements while we are doing this.
    _flattenedAppendedObj.clear();
    if (!traverseLockStep(
            _referenceSubObj, obj, [this](const BSONElement& ref, const BSONElement& elem) {
                uassert(ErrorCodes::InvalidBSONType,
                        "MinKey or MaxKey is not valid for storage",
                        elem.type() != MinKey && elem.type() != MaxKey);
                _flattenedAppendedObj.push_back(elem);
            })) {
        _flushSubObjMode();
        _startDetermineSubObjReference(obj);
        return;
    }

    // We should have recieved one callback for every sub-element in reference object. This should
    // match number of encoding states setup previously.
    invariant(_flattenedAppendedObj.size() == _subobjStates.size());
    auto statesIt = _subobjStates.begin();
    auto subElemIt = _flattenedAppendedObj.begin();
    auto subElemEnd = _flattenedAppendedObj.end();

    // Append elements to corresponding encoding state.
    for (; subElemIt != subElemEnd; ++subElemIt, ++statesIt) {
        const auto& subelem = *subElemIt;
        auto& state = *statesIt;
        if (!subelem.eoo())
            state.append(subelem);
        else
            state.skip();
    }
}

void BSONColumnBuilder::_startDetermineSubObjReference(const BSONObj& obj) {
    // Start sub-object compression. Enter DeterminingReference mode, we use this first Object
    // as the first reference
    _state.flush();
    _state = {&_bufBuilder, nullptr};

    _traverse(obj, [](const BSONElement& elem, const BSONElement&) {
        uassert(ErrorCodes::InvalidBSONType,
                "MinKey or MaxKey is not valid for storage",
                elem.type() != MinKey && elem.type() != MaxKey);
    });

    _referenceSubObj = obj.getOwned();
    _bufferedObjElements.push_back(_referenceSubObj);
    _mode = Mode::kSubObjDeterminingReference;
}

void BSONColumnBuilder::_finishDetermineSubObjReference() {
    // Done determining reference sub-object. Write this control byte and object to stream.
    _bufBuilder.appendChar(bsoncolumn::kInterleavedStartControlByte);
    _bufBuilder.appendBuf(_referenceSubObj.objdata(), _referenceSubObj.objsize());

    // Initialize all encoding states. We do this by traversing in lock-step between the reference
    // object and first buffered element. We can use the fact if sub-element exists in reference to
    // determine if we should start with a zero delta or skip.
    bool res =
        traverseLockStep(_referenceSubObj,
                         _bufferedObjElements.front(),
                         [this](const BSONElement& ref, const BSONElement& elem) {
                             _subobjBuffers.emplace_back();
                             auto* buffer = &_subobjBuffers.back().first;
                             auto* controlBlocks = &_subobjBuffers.back().second;

                             // We need to buffer all control blocks written by the EncodingStates
                             // so they can be added to the main buffer in the right order.
                             auto controlBlockWriter = [buffer, controlBlocks](
                                                           const char* controlBlock, size_t size) {
                                 controlBlocks->emplace_back(controlBlock - buffer->buf(), size);
                             };

                             // Set a valid 'previous' into the encoding state to avoid a full
                             // literal to be written when we append the first element. We want this
                             // to be a zero delta as the reference object already contain this
                             // literal.
                             _subobjStates.emplace_back(buffer, controlBlockWriter);
                             _subobjStates.back()._storePrevious(ref);
                             _subobjStates.back()._initializeFromPrevious();
                             if (!elem.eoo()) {
                                 _subobjStates.back().append(elem);
                             } else {
                                 _subobjStates.back().skip();
                             }
                         });
    invariant(res);
    _mode = Mode::kSubObjAppending;

    // Append remaining buffered objects.
    auto it = _bufferedObjElements.begin() + 1;
    auto end = _bufferedObjElements.end();
    for (; it != end; ++it) {
        _appendSubElements(*it);
    }
    _bufferedObjElements.clear();
}

void BSONColumnBuilder::_flushSubObjMode() {
    if (_mode == Mode::kSubObjDeterminingReference) {
        _finishDetermineSubObjReference();
    }

    // Flush all EncodingStates, this will cause them to write out all their elements that is
    // captured by the controlBlockWriter.
    for (auto&& state : _subobjStates) {
        state.flush();
    }

    // We now need to write all control blocks to the binary stream in the right order. This is done
    // in the decoder's perspective where a DecodingState that exhausts its elements will read the
    // next control byte. We can use a min-heap to see which encoding states have written the fewest
    // elements so far. In case of tie we use the smallest encoder/decoder index.
    std::vector<std::pair<uint32_t /* num elements written */, uint32_t /* encoder index */>> heap;
    for (uint32_t i = 0; i < _subobjBuffers.size(); ++i) {
        heap.emplace_back(0, i);
    }

    // Initialize as min-heap
    using MinHeap = std::greater<std::pair<uint32_t, uint32_t>>;
    std::make_heap(heap.begin(), heap.end(), MinHeap());

    // Append all control blocks
    while (!heap.empty()) {
        // Take out encoding state with fewest elements written from heap
        std::pop_heap(heap.begin(), heap.end(), MinHeap());
        // And we take out control blocks in FIFO order from this encoding state
        auto& slot = _subobjBuffers[heap.back().second];
        const char* controlBlock = slot.first.buf() + slot.second.front().first;
        size_t size = slot.second.front().second;

        // Write it to the buffer
        _bufBuilder.appendBuf(controlBlock, size);
        slot.second.pop_front();
        if (slot.second.empty()) {
            // No more control blocks for this encoding state so remove it from the heap
            heap.pop_back();
            continue;
        }

        // Calculate how many elements were in this control block
        uint32_t elems = [&]() -> uint32_t {
            if (bsoncolumn::isLiteralControlByte(*controlBlock)) {
                return 1;
            }

            Simple8b<uint128_t> reader(
                controlBlock + 1,
                sizeof(uint64_t) * bsoncolumn::numSimple8bBlocksForControlByte(*controlBlock));

            uint32_t num = 0;
            auto it = reader.begin();
            auto end = reader.end();
            while (it != end) {
                num += it.blockSize();
                it.advanceBlock();
            }
            return num;
        }();

        // Append num elements and put this encoding state back into the heap.
        heap.back().first += elems;
        std::push_heap(heap.begin(), heap.end(), MinHeap());
    }
    // All control blocks written, write EOO to end the interleaving and cleanup.
    _bufBuilder.appendChar(EOO);
    _subobjStates.clear();
    _subobjBuffers.clear();
    _mode = Mode::kRegular;
}

}  // namespace mongo
