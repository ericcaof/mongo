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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"

#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"

namespace mongo::timeseries {

namespace {

bool isIndexOnControl(const StringData& field) {
    return field.startsWith(timeseries::kControlMinFieldNamePrefix) ||
        field.startsWith(timeseries::kControlMaxFieldNamePrefix);
}

/**
 * Takes the index specification field name, such as 'control.max.x.y', or 'control.min.z' and
 * returns a pair of the prefix ('control.min.' or 'control.max.') and key ('x.y' or 'z').
 */
std::pair<std::string, std::string> extractControlPrefixAndKey(const StringData& field) {
    // Can't use rfind() due to dotted fields such as 'control.max.x.y'.
    size_t numDotsFound = 0;
    auto fieldIt = std::find_if(field.begin(), field.end(), [&numDotsFound](const char c) {
        if (c == '.') {
            numDotsFound++;
        }

        return numDotsFound == 2;
    });

    invariant(numDotsFound == 2 && fieldIt != field.end());
    return {std::string(field.begin(), fieldIt + 1), std::string(fieldIt + 1, field.end())};
}

/**
 * Converts an event-level index spec to a bucket-level index spec.
 *
 * If the input is not a valid index spec, this function must either:
 *  - return an error Status
 *  - return an invalid index spec
 */
StatusWith<BSONObj> createBucketsSpecFromTimeseriesSpec(const TimeseriesOptions& timeseriesOptions,
                                                        const BSONObj& timeseriesIndexSpecBSON,
                                                        bool isShardKeySpec) {
    if (timeseriesIndexSpecBSON.isEmpty()) {
        return {ErrorCodes::BadValue, "Empty object is not a valid index spec"_sd};
    }
    if (timeseriesIndexSpecBSON.firstElement().fieldNameStringData() == "$hint"_sd ||
        timeseriesIndexSpecBSON.firstElement().fieldNameStringData() == "$natural"_sd) {
        return {
            ErrorCodes::BadValue,
            str::stream() << "Invalid index spec (perhaps it's a valid hint, that was incorrectly "
                          << "passed to createBucketsSpecFromTimeseriesSpec): "
                          << timeseriesIndexSpecBSON};
    }

    auto timeField = timeseriesOptions.getTimeField();
    auto metaField = timeseriesOptions.getMetaField();

    BSONObjBuilder builder;
    for (const auto& elem : timeseriesIndexSpecBSON) {
        if (elem.fieldNameStringData() == timeField) {
            // The index requested on the time field must be a number for an ascending or descending
            // index specification. Note: further validation is expected of the caller, such as
            // eventually calling index_key_validate::validateKeyPattern() on the spec.
            if (!elem.isNumber()) {
                return {ErrorCodes::BadValue,
                        str::stream()
                            << "Invalid index spec for time-series collection: "
                            << redact(timeseriesIndexSpecBSON)
                            << ". Indexes on the time field must be ascending or descending "
                               "(numbers only): "
                            << elem};
            }

            // The time-series index on the 'timeField' is converted into a compound time index on
            // the buckets collection for more efficient querying of buckets.
            if (elem.number() >= 0) {
                builder.appendAs(
                    elem, str::stream() << timeseries::kControlMinFieldNamePrefix << timeField);
                if (!isShardKeySpec) {
                    builder.appendAs(
                        elem, str::stream() << timeseries::kControlMaxFieldNamePrefix << timeField);
                }
            } else {
                builder.appendAs(
                    elem, str::stream() << timeseries::kControlMaxFieldNamePrefix << timeField);
                builder.appendAs(
                    elem, str::stream() << timeseries::kControlMinFieldNamePrefix << timeField);
            }
            continue;
        }

        if (metaField) {
            if (elem.fieldNameStringData() == *metaField) {
                // The time-series 'metaField' field name always maps to a field named
                // timeseries::kBucketMetaFieldName on the underlying buckets collection.
                builder.appendAs(elem, timeseries::kBucketMetaFieldName);
                continue;
            }

            // Time-series indexes on sub-documents of the 'metaField' are allowed.
            if (elem.fieldNameStringData().startsWith(*metaField + ".")) {
                builder.appendAs(elem,
                                 str::stream()
                                     << timeseries::kBucketMetaFieldName << "."
                                     << elem.fieldNameStringData().substr(metaField->size() + 1));
                continue;
            }
        }

        // Indexes on measurement fields are only supported when the 'gTimeseriesMetricIndexes'
        // feature flag is enabled.
        if (!feature_flags::gTimeseriesMetricIndexes.isEnabledAndIgnoreFCV()) {
            auto reason = str::stream();
            reason << "Invalid index spec for time-series collection: "
                   << redact(timeseriesIndexSpecBSON) << ". ";
            reason << "Indexes are only supported on the '" << timeField << "' ";
            if (metaField) {
                reason << "and '" << *metaField << "' fields. ";
            } else {
                reason << "field. ";
            }
            reason << "Attempted to create an index on the field '" << elem.fieldName() << "'.";
            return {ErrorCodes::BadValue, reason};
        }

        // 2dsphere indexes on measurements are allowed, but need to be re-written to
        // point to the data field and use the special 2dsphere_bucket index type.
        if (elem.valueStringData() == IndexNames::GEO_2DSPHERE) {
            builder.append(str::stream() << timeseries::kBucketDataFieldName << "."
                                         << elem.fieldNameStringData(),
                           IndexNames::GEO_2DSPHERE_BUCKET);
            continue;
        }

        // No other special index types are allowed on timeseries measurements.
        if (!elem.isNumber()) {
            return {
                ErrorCodes::BadValue,
                str::stream() << "Invalid index spec for time-series collection: "
                              << redact(timeseriesIndexSpecBSON)
                              << ". Indexes on measurement fields must be ascending or descending "
                                 "(numbers only), or '2dsphere': "
                              << elem};
        }

        if (elem.number() >= 0) {
            // For ascending key patterns, the { control.min.elem: 1, control.max.elem: 1 }
            // compound index is created.
            builder.appendAs(
                elem, str::stream() << timeseries::kControlMinFieldNamePrefix << elem.fieldName());
            builder.appendAs(
                elem, str::stream() << timeseries::kControlMaxFieldNamePrefix << elem.fieldName());
        } else if (elem.number() < 0) {
            // For descending key patterns, the { control.max.elem: -1, control.min.elem: -1 }
            // compound index is created.
            builder.appendAs(
                elem, str::stream() << timeseries::kControlMaxFieldNamePrefix << elem.fieldName());
            builder.appendAs(
                elem, str::stream() << timeseries::kControlMinFieldNamePrefix << elem.fieldName());
        }
    }

    return builder.obj();
}

/**
 * Maps the buckets collection index spec 'bucketsIndexSpecBSON' to the index schema of the
 * time-series collection using the information provided in 'timeseriesOptions'.
 *
 * If 'bucketsIndexSpecBSON' does not match a valid time-series index format, then boost::none is
 * returned.
 *
 * Conversion Example:
 * On a time-series collection with 'tm' time field and 'mm' metadata field,
 * we may see a compound index on the underlying bucket collection mapped from:
 * {
 *     'meta.tag1': 1,
 *     'control.min.tm': 1,
 *     'control.max.tm': 1
 * }
 * to an index on the time-series collection:
 * {
 *     'mm.tag1': 1,
 *     'tm': 1
 * }
 */
boost::optional<BSONObj> createTimeseriesIndexSpecFromBucketsIndexSpec(
    const TimeseriesOptions& timeseriesOptions,
    const BSONObj& bucketsIndexSpecBSON,
    bool timeseriesMetricIndexesFeatureFlagEnabled) {
    auto timeField = timeseriesOptions.getTimeField();
    auto metaField = timeseriesOptions.getMetaField();

    const std::string controlMinTimeField = str::stream()
        << timeseries::kControlMinFieldNamePrefix << timeField;
    const std::string controlMaxTimeField = str::stream()
        << timeseries::kControlMaxFieldNamePrefix << timeField;

    BSONObjBuilder builder;
    for (auto elemIt = bucketsIndexSpecBSON.begin(); elemIt != bucketsIndexSpecBSON.end();
         ++elemIt) {
        const auto& elem = *elemIt;
        // The index specification on the time field is ascending or descending.
        if (elem.fieldNameStringData() == controlMinTimeField) {
            if (!elem.isNumber()) {
                // This index spec on the underlying buckets collection is not valid for
                // time-series. Therefore, we will not convert the index spec.
                return {};
            }

            builder.appendAs(elem, timeField);
            continue;
        } else if (elem.fieldNameStringData() == controlMaxTimeField) {
            // Skip 'control.max.<timeField>' since the 'control.min.<timeField>' field is
            // sufficient to determine whether the index is ascending or descending.

            continue;
        }

        if (metaField) {
            if (elem.fieldNameStringData() == timeseries::kBucketMetaFieldName) {
                builder.appendAs(elem, *metaField);
                continue;
            }

            if (elem.fieldNameStringData().startsWith(timeseries::kBucketMetaFieldName + ".")) {
                builder.appendAs(elem,
                                 str::stream() << *metaField << "."
                                               << elem.fieldNameStringData().substr(
                                                      timeseries::kBucketMetaFieldName.size() + 1));
                continue;
            }
        }

        if (!timeseriesMetricIndexesFeatureFlagEnabled) {
            // 'elem' is an invalid index spec field for this time-series collection. It matches
            // neither the time field nor the metaField field. Therefore, we will not convert the
            // index spec.
            return {};
        }

        if (elem.fieldNameStringData().startsWith(timeseries::kBucketDataFieldName + ".") &&
            elem.valueStringData() == IndexNames::GEO_2DSPHERE_BUCKET) {
            builder.append(
                elem.fieldNameStringData().substr(timeseries::kBucketDataFieldName.size() + 1),
                IndexNames::GEO_2DSPHERE);
            continue;
        }

        if (!isIndexOnControl(elem.fieldNameStringData())) {
            // Only indexes on the control field are allowed beyond this point. We will not convert
            // the index spec.
            return {};
        }

        // Indexes on measurement fields are built as compound indexes on the two 'control.min' and
        // 'control.max' fields. We use the BSON iterator to lookahead when doing the reverse
        // mapping for these indexes.
        const auto firstOrdering = elem.number();
        std::string firstControlFieldPrefix;
        std::string firstControlFieldKey;
        std::tie(firstControlFieldPrefix, firstControlFieldKey) =
            extractControlPrefixAndKey(elem.fieldNameStringData());

        elemIt++;
        if (elemIt == bucketsIndexSpecBSON.end()) {
            // This measurement index spec on the underlying buckets collection is not valid for
            // time-series as the compound index is incomplete. We will not convert the index spec.
            return {};
        }

        const auto& nextElem = *elemIt;
        if (!isIndexOnControl(nextElem.fieldNameStringData())) {
            // Only indexes on the control field are allowed beyond this point. We will not convert
            // the index spec.
            return {};
        }

        const auto secondOrdering = nextElem.number();
        std::string secondControlFieldPrefix;
        std::string secondControlFieldKey;
        std::tie(secondControlFieldPrefix, secondControlFieldKey) =
            extractControlPrefixAndKey(nextElem.fieldNameStringData());

        if (firstOrdering != secondOrdering) {
            // The compound index has a mixed ascending and descending key pattern. Do not convert
            // the index spec.
            return {};
        }

        if (firstControlFieldPrefix == timeseries::kControlMinFieldNamePrefix &&
            secondControlFieldPrefix == timeseries::kControlMaxFieldNamePrefix &&
            firstControlFieldKey == secondControlFieldKey && firstOrdering >= 0) {
            // Ascending index.
            builder.appendAs(nextElem, firstControlFieldKey);
            continue;
        } else if (firstControlFieldPrefix == timeseries::kControlMaxFieldNamePrefix &&
                   secondControlFieldPrefix == timeseries::kControlMinFieldNamePrefix &&
                   firstControlFieldKey == secondControlFieldKey && firstOrdering < 0) {
            // Descending index.
            builder.appendAs(nextElem, firstControlFieldKey);
            continue;
        } else {
            // This measurement index spec on the underlying buckets collection is not valid for
            // time-series as the compound index has the wrong ordering. We will not convert the
            // index spec.
            return {};
        }
    }

    return builder.obj();
}

}  // namespace

StatusWith<BSONObj> createBucketsIndexSpecFromTimeseriesIndexSpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& timeseriesIndexSpecBSON) {
    return createBucketsSpecFromTimeseriesSpec(timeseriesOptions, timeseriesIndexSpecBSON, false);
}

StatusWith<BSONObj> createBucketsShardKeySpecFromTimeseriesShardKeySpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& timeseriesShardKeySpecBSON) {
    return createBucketsSpecFromTimeseriesSpec(timeseriesOptions, timeseriesShardKeySpecBSON, true);
}

boost::optional<BSONObj> createTimeseriesIndexFromBucketsIndex(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& bucketsIndex) {
    bool timeseriesMetricIndexesFeatureFlagEnabled =
        feature_flags::gTimeseriesMetricIndexes.isEnabledAndIgnoreFCV();

    if (bucketsIndex.hasField(kOriginalSpecFieldName) &&
        timeseriesMetricIndexesFeatureFlagEnabled) {
        // This buckets index has the original user index definition available, return it if the
        // time-series metric indexes feature flag is enabled. If the feature flag isn't enabled,
        // the reverse mapping mechanism will be used. This is necessary to skip returning any
        // incompatible indexes created when the feature flag was enabled.
        return bucketsIndex.getObjectField(kOriginalSpecFieldName);
    }
    if (bucketsIndex.hasField(kKeyFieldName)) {
        auto timeseriesKeyValue = createTimeseriesIndexSpecFromBucketsIndexSpec(
            timeseriesOptions,
            bucketsIndex.getField(kKeyFieldName).Obj(),
            timeseriesMetricIndexesFeatureFlagEnabled);
        if (timeseriesKeyValue) {
            // This creates a BSONObj copy with the kOriginalSpecFieldName field removed, if it
            // exists, and modifies the kKeyFieldName field to timeseriesKeyValue.
            BSONObj intermediateObj =
                bucketsIndex.removeFields(StringDataSet{kOriginalSpecFieldName});
            return intermediateObj.addFields(BSON(kKeyFieldName << timeseriesKeyValue.get()),
                                             StringDataSet{kKeyFieldName});
        }
    }
    return boost::none;
}

std::list<BSONObj> createTimeseriesIndexesFromBucketsIndexes(
    const TimeseriesOptions& timeseriesOptions, const std::list<BSONObj>& bucketsIndexes) {
    std::list<BSONObj> indexSpecs;
    for (const auto& bucketsIndex : bucketsIndexes) {
        auto timeseriesIndex =
            createTimeseriesIndexFromBucketsIndex(timeseriesOptions, bucketsIndex);
        if (timeseriesIndex) {
            indexSpecs.push_back(timeseriesIndex->getOwned());
        }
    }
    return indexSpecs;
}

bool isBucketsIndexSpecCompatibleForDowngrade(const TimeseriesOptions& timeseriesOptions,
                                              const BSONObj& bucketsIndex) {
    if (!bucketsIndex.hasField(kKeyFieldName)) {
        return false;
    }

    if (bucketsIndex.hasField(kPartialFilterExpressionFieldName)) {
        // Partial indexes are not supported in FCV < 5.2.
        return false;
    }

    return createTimeseriesIndexSpecFromBucketsIndexSpec(
               timeseriesOptions,
               bucketsIndex.getField(kKeyFieldName).Obj(),
               /*timeseriesMetricIndexesFeatureFlagEnabled=*/false) != boost::none;
}

bool doesBucketsIndexIncludeMeasurement(OperationContext* opCtx,
                                        const NamespaceString& bucketNs,
                                        const TimeseriesOptions& timeseriesOptions,
                                        const BSONObj& bucketsIndex) {
    tassert(5916306,
            str::stream() << "Index spec has no 'key': " << bucketsIndex.toString(),
            bucketsIndex.hasField(kKeyFieldName));

    auto timeField = timeseriesOptions.getTimeField();
    auto metaField = timeseriesOptions.getMetaField();

    const std::string controlMinTimeField = str::stream()
        << timeseries::kControlMinFieldNamePrefix << timeField;
    const std::string controlMaxTimeField = str::stream()
        << timeseries::kControlMaxFieldNamePrefix << timeField;
    static const std::string idField = "_id";

    auto isMeasurementField = [&](StringData name) -> bool {
        if (name == controlMinTimeField || name == controlMaxTimeField) {
            return false;
        }

        if (metaField) {
            if (name == timeseries::kBucketMetaFieldName ||
                name.startsWith(timeseries::kBucketMetaFieldName + ".")) {
                return false;
            }
        }

        return true;
    };

    // Check index key.
    const BSONObj keyObj = bucketsIndex.getField(kKeyFieldName).Obj();
    for (const auto& elem : keyObj) {
        if (isMeasurementField(elem.fieldNameStringData()))
            return true;
    }

    // Check partial filter expression.
    if (auto filterElem = bucketsIndex[kPartialFilterExpressionFieldName]) {
        tassert(5916302,
                str::stream() << "Partial filter expression is not an object: " << filterElem,
                filterElem.type() == BSONType::Object);

        auto expCtx = make_intrusive<ExpressionContext>(opCtx, nullptr /* collator */, bucketNs);

        MatchExpressionParser::AllowedFeatureSet allowedFeatures =
            MatchExpressionParser::kDefaultSpecialFeatures;

        // TODO SERVER-53380 convert to tassertStatusOK.
        auto statusWithFilter = MatchExpressionParser::parse(
            filterElem.Obj(), expCtx, ExtensionsCallbackNoop{}, allowedFeatures);
        tassert(5916303,
                str::stream() << "Partial filter expression failed to parse: "
                              << statusWithFilter.getStatus(),
                statusWithFilter.isOK());
        auto filter = std::move(statusWithFilter.getValue());

        if (!expression::isOnlyDependentOn(*filter,
                                           {std::string{timeseries::kBucketMetaFieldName},
                                            controlMinTimeField,
                                            controlMaxTimeField,
                                            idField})) {
            // Partial filter expression depends on a non-time, non-metadata field.
            return true;
        }
    }

    return false;
}

bool isHintIndexKey(const BSONObj& obj) {
    if (obj.isEmpty())
        return false;
    StringData fieldName = obj.firstElement().fieldNameStringData();
    if (fieldName == "$hint"_sd)
        return false;
    if (fieldName == "$natural"_sd)
        return false;

    return true;
}

}  // namespace mongo::timeseries
