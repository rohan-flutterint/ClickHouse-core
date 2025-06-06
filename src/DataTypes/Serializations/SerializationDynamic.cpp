#include <DataTypes/Serializations/SerializationDynamic.h>
#include <DataTypes/Serializations/SerializationVariant.h>
#include <DataTypes/Serializations/SerializationDynamicHelpers.h>
#include <DataTypes/FieldToDataType.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeVariant.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypeNothing.h>
#include <DataTypes/DataTypesBinaryEncoding.h>

#include <Columns/ColumnDynamic.h>
#include <Columns/ColumnString.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadBufferFromString.h>
#include <Interpreters/castColumn.h>
#include <Formats/EscapingRuleUtils.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
    extern const int LOGICAL_ERROR;
}

struct SerializeBinaryBulkStateDynamic : public ISerialization::SerializeBinaryBulkState
{
    SerializationDynamic::DynamicSerializationVersion structure_version;
    size_t num_dynamic_types;
    DataTypePtr variant_type;
    Names variant_names;
    SerializationPtr variant_serialization;
    ISerialization::SerializeBinaryBulkStatePtr variant_state;

    /// Variants statistics.
    ColumnDynamic::Statistics statistics;
    /// If true, statistics will be recalculated during serialization.
    bool recalculate_statistics = false;

    /// For flattened serialization only.
    std::optional<FlattenedDynamicColumn> flattened_column;
    std::vector<ISerialization::SerializeBinaryBulkStatePtr> flattened_states;
    ISerialization::SerializeBinaryBulkStatePtr flattened_indexes_state;

    explicit SerializeBinaryBulkStateDynamic(UInt64 structure_version_)
        : structure_version(structure_version_), statistics(ColumnDynamic::Statistics::Source::READ)
    {
    }
};

struct DeserializeBinaryBulkStateDynamic : public ISerialization::DeserializeBinaryBulkState
{
    SerializationPtr variant_serialization;
    ISerialization::DeserializeBinaryBulkStatePtr variant_state;
    ISerialization::DeserializeBinaryBulkStatePtr structure_state;

    /// For flattened serialization only.
    std::vector<ISerialization::DeserializeBinaryBulkStatePtr> flattened_states;
    ISerialization::DeserializeBinaryBulkStatePtr flattened_indexes_state;

    ISerialization::DeserializeBinaryBulkStatePtr clone() const override
    {
        auto new_state = std::make_shared<DeserializeBinaryBulkStateDynamic>(*this);
        new_state->variant_state = variant_state ? variant_state->clone() : nullptr;
        new_state->structure_state = structure_state ? structure_state->clone() : nullptr;
        return new_state;
    }
};

void SerializationDynamic::enumerateStreams(
    EnumerateStreamsSettings & settings,
    const StreamCallback & callback,
    const SubstreamData & data) const
{
    settings.path.push_back(Substream::DynamicStructure);
    callback(settings.path);
    settings.path.pop_back();

    const auto * column_dynamic = data.column ? &assert_cast<const ColumnDynamic &>(*data.column) : nullptr;
    const auto * deserialize_state = data.deserialize_state ? checkAndGetState<DeserializeBinaryBulkStateDynamic>(data.deserialize_state) : nullptr;

    /// If column is nullptr and we don't have deserialize state yet, nothing to enumerate as we don't have any variants.
    if (!settings.enumerate_dynamic_streams || (!column_dynamic && !deserialize_state))
        return;

    const auto & variant_type = column_dynamic ? column_dynamic->getVariantInfo().variant_type : checkAndGetState<DeserializeBinaryBulkStateDynamicStructure>(deserialize_state->structure_state)->variant_type;
    auto variant_serialization = variant_type->getDefaultSerialization();

    settings.path.push_back(Substream::DynamicData);
    auto variant_data = SubstreamData(variant_serialization)
                         .withType(variant_type)
                         .withColumn(column_dynamic ? column_dynamic->getVariantColumnPtr() : nullptr)
                         .withSerializationInfo(data.serialization_info)
                         .withDeserializeState(deserialize_state ? deserialize_state->variant_state : nullptr);
    settings.path.back().data = variant_data;
    variant_serialization->enumerateStreams(settings, callback, variant_data);
    settings.path.pop_back();
}

SerializationDynamic::DynamicSerializationVersion::DynamicSerializationVersion(UInt64 version) : value(static_cast<Value>(version))
{
    checkVersion(version);
}

void SerializationDynamic::DynamicSerializationVersion::checkVersion(UInt64 version)
{
    if (version != V1 && version != V2 && version != FLATTENED)
        throw Exception(ErrorCodes::INCORRECT_DATA, "Invalid version for Dynamic structure serialization: {}", version);
}

void SerializationDynamic::serializeBinaryBulkStatePrefix(
    const DB::IColumn & column,
    SerializeBinaryBulkSettings & settings,
    SerializeBinaryBulkStatePtr & state) const
{
    const auto & column_dynamic = assert_cast<const ColumnDynamic &>(column);
    const auto & variant_info = column_dynamic.getVariantInfo();

    settings.path.push_back(Substream::DynamicStructure);
    auto * stream = settings.getter(settings.path);
    settings.path.pop_back();

    if (!stream)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Missing stream for Dynamic column structure during serialization of binary bulk state prefix");

    /// Choose serialization type.
    /// By default we use serialization V2.
    UInt64 structure_version = DynamicSerializationVersion::Value::V2;
    /// Check if we are writing data in Native format and have FLATTENED serialization enabled.
    if (settings.native_format && settings.format_settings && settings.format_settings->native.use_flattened_dynamic_and_json_serialization)
        structure_version = DynamicSerializationVersion::Value::FLATTENED;
    /// Check if we should use V1 serialization for compatibility.
    else if (settings.use_v1_object_and_dynamic_serialization)
        structure_version = DynamicSerializationVersion::Value::V1;

    /// Write selected structure serialization version.
    writeBinaryLittleEndian(structure_version, *stream);

    auto dynamic_state = std::make_shared<SerializeBinaryBulkStateDynamic>(structure_version);
    if (structure_version == DynamicSerializationVersion::Value::FLATTENED)
    {
        auto flattened_column = flattenDynamicColumn(column_dynamic);
        /// Write the list of all flattened types.
        writeVarUInt(flattened_column.types.size(), *stream);
        for (const auto & type : flattened_column.types)
        {
            if (settings.native_format && settings.format_settings && settings.format_settings->native.encode_types_in_binary_format)
                encodeDataType(type);
            else
                writeStringBinary(type->getName(), *stream);
        }

        /// Write prefixes for indexes and all flattened types.
        settings.path.push_back(Substream::DynamicData);
        auto indexes_serialization = flattened_column.indexes_type->getDefaultSerialization();
        indexes_serialization->serializeBinaryBulkStatePrefix(*flattened_column.indexes_column, settings, dynamic_state->flattened_indexes_state);
        for (size_t i = 0; i != flattened_column.types.size(); ++i)
        {
            auto serialization = flattened_column.types[i]->getDefaultSerialization();
            dynamic_state->flattened_states.emplace_back();
            serialization->serializeBinaryBulkStatePrefix(*flattened_column.columns[i], settings, dynamic_state->flattened_states.back());
        }
        settings.path.pop_back();

        dynamic_state->flattened_column = std::move(flattened_column);
        state = std::move(dynamic_state);
        return;
    }

    dynamic_state->variant_type = variant_info.variant_type;
    dynamic_state->variant_names = variant_info.variant_names;
    const auto & variant_column = column_dynamic.getVariantColumn();

    /// Write information about dynamic types.
    dynamic_state->num_dynamic_types = dynamic_state->variant_names.size() - 1; ///  -1 for SharedVariant

    /// In V1 version we had max_dynamic_types parameter written, but now we need only actual number of variants.
    /// For compatibility we need to write V1 version sometimes, but we should write number of variants instead of
    /// max_dynamic_types (because now max_dynamic_types can be different in different serialized columns).
    if (structure_version == DynamicSerializationVersion::Value::V1)
        writeVarUInt(dynamic_state->num_dynamic_types, *stream);

    writeVarUInt(dynamic_state->num_dynamic_types, *stream);
    if (settings.native_format && settings.format_settings && settings.format_settings->native.encode_types_in_binary_format)
    {
        const auto & variants = assert_cast<const DataTypeVariant &>(*dynamic_state->variant_type).getVariants();
        for (const auto & variant: variants)
        {
            if (variant->getName() != ColumnDynamic::getSharedVariantTypeName())
                encodeDataType(variant, *stream);
        }
    }
    else
    {
        for (const auto & name : dynamic_state->variant_names)
        {
            if (name != ColumnDynamic::getSharedVariantTypeName())
                writeStringBinary(name, *stream);
        }
    }

    /// Write statistics in prefix if needed.
    if (settings.object_and_dynamic_write_statistics == SerializeBinaryBulkSettings::ObjectAndDynamicStatisticsMode::PREFIX)
    {
        const auto & statistics = column_dynamic.getStatistics();
        /// First, write statistics for usual variants.
        for (size_t i = 0; i != variant_info.variant_names.size(); ++i)
        {
            size_t size = 0;
            /// Check if we can use statistics stored in the column. There are 2 possible sources
            /// of this statistics:
            ///   - statistics calculated during merge of some data parts (Statistics::Source::MERGE)
            ///   - statistics read from the data part during deserialization of Dynamic column (Statistics::Source::READ).
            /// We can rely only on statistics calculated during the merge, because column with statistics that was read
            /// during deserialization from some data part could be filtered/limited/transformed/etc and so the statistics can be outdated.
            if (statistics && statistics->source == ColumnDynamic::Statistics::Source::MERGE)
                size = statistics->variants_statistics.at(variant_info.variant_names[i]);
            /// Otherwise we can use only variant sizes from current column.
            else
                size = variant_column.getVariantByGlobalDiscriminator(i).size();
            writeVarUInt(size, *stream);
        }

        /// Second, write statistics for variants in shared variant.
        /// Check if we have statistics calculated during merge of some data parts (Statistics::Source::MERGE).
        if (statistics && statistics->source == ColumnDynamic::Statistics::Source::MERGE)
        {
            writeVarUInt(statistics->shared_variants_statistics.size(), *stream);
            for (const auto & [variant_name, size] : statistics->shared_variants_statistics)
            {
                writeStringBinary(variant_name, *stream);
                writeVarUInt(size, *stream);
            }
        }
        /// If we don't have statistics for shared variants from merge, calculate it from the column.
        else
        {
            std::unordered_map<String, size_t> shared_variants_statistics;
            const auto & shared_variant = column_dynamic.getSharedVariant();
            for (size_t i = 0; i != shared_variant.size(); ++i)
            {
                auto value = shared_variant.getDataAt(i);
                ReadBufferFromMemory buf(value.data, value.size);
                auto type = decodeDataType(buf);
                auto type_name = type->getName();
                if (auto it = shared_variants_statistics.find(type_name); it != shared_variants_statistics.end())
                    ++it->second;
                else if (shared_variants_statistics.size() < ColumnDynamic::Statistics::MAX_SHARED_VARIANT_STATISTICS_SIZE)
                    shared_variants_statistics.emplace(type_name, 1);
            }

            writeVarUInt(shared_variants_statistics.size(), *stream);
            for (const auto & [variant_name, size] : shared_variants_statistics)
            {
                writeStringBinary(variant_name, *stream);
                writeVarUInt(size, *stream);
            }
        }
    }
    /// Otherwise statistics will be written in the suffix, in this case we will recalculate
    /// statistics during serialization to make it more precise.
    else
    {
        dynamic_state->recalculate_statistics = true;
    }

    dynamic_state->variant_serialization = dynamic_state->variant_type->getDefaultSerialization();
    settings.path.push_back(Substream::DynamicData);
    dynamic_state->variant_serialization->serializeBinaryBulkStatePrefix(variant_column, settings, dynamic_state->variant_state);
    settings.path.pop_back();

    state = std::move(dynamic_state);
}

void SerializationDynamic::deserializeBinaryBulkStatePrefix(
    DeserializeBinaryBulkSettings & settings,
    DeserializeBinaryBulkStatePtr & state,
    SubstreamsDeserializeStatesCache * cache) const
{
    DeserializeBinaryBulkStatePtr structure_state = deserializeDynamicStructureStatePrefix(settings, cache);
    if (!structure_state)
        return;

    auto dynamic_state = std::make_shared<DeserializeBinaryBulkStateDynamic>();
    dynamic_state->structure_state = std::move(structure_state);
    auto * structure_state_typed = checkAndGetState<DeserializeBinaryBulkStateDynamicStructure>(dynamic_state->structure_state);
    if (structure_state_typed->structure_version.value == DynamicSerializationVersion::Value::FLATTENED)
    {
        dynamic_state->flattened_states.reserve(structure_state_typed->flattened_data_types.size());
        /// Read prefix of indexes and all flattened types.
        settings.path.push_back(Substream::DynamicData);
        structure_state_typed->flattened_indexes_type->getDefaultSerialization()->deserializeBinaryBulkStatePrefix(settings, dynamic_state->flattened_indexes_state, cache);
        for (const auto & type : structure_state_typed->flattened_data_types)
        {
            dynamic_state->flattened_states.emplace_back();
            type->getDefaultSerialization()->deserializeBinaryBulkStatePrefix(settings, dynamic_state->flattened_states.back(), cache);
        }
        settings.path.pop_back();
        state = std::move(dynamic_state);
        return;
    }

    dynamic_state->variant_serialization = structure_state_typed->variant_type->getDefaultSerialization();

    settings.path.push_back(Substream::DynamicData);

    /// Call callback for newly discovered dynamic subcolumns if needed.
    if (settings.dynamic_subcolumns_callback)
    {
        EnumerateStreamsSettings enumerate_settings;
        enumerate_settings.path = settings.path;
        dynamic_state->variant_serialization->enumerateStreams(enumerate_settings, settings.dynamic_subcolumns_callback, SubstreamData(dynamic_state->variant_serialization));
    }

    dynamic_state->variant_serialization->deserializeBinaryBulkStatePrefix(settings, dynamic_state->variant_state, cache);
    settings.path.pop_back();

    state = std::move(dynamic_state);
}

ISerialization::DeserializeBinaryBulkStatePtr SerializationDynamic::deserializeDynamicStructureStatePrefix(
    DeserializeBinaryBulkSettings & settings, SubstreamsDeserializeStatesCache * cache)
{
    settings.path.push_back(Substream::DynamicStructure);

    DeserializeBinaryBulkStatePtr state = nullptr;
    if (auto cached_state = getFromSubstreamsDeserializeStatesCache(cache, settings.path))
    {
        state = std::move(cached_state);
    }
    else if (auto * structure_stream = settings.getter(settings.path))
    {
        /// Read structure serialization version.
        UInt64 structure_version;
        readBinaryLittleEndian(structure_version, *structure_stream);
        auto structure_state = std::make_shared<DeserializeBinaryBulkStateDynamicStructure>(structure_version);
        if (structure_state->structure_version.value == DynamicSerializationVersion::Value::FLATTENED)
        {
            /// Read the flattened list of types.
            size_t num_types;
            readVarUInt(num_types, *structure_stream);
            structure_state->flattened_data_types.reserve(num_types);
            String data_type_name;
            for (size_t i = 0; i != num_types; ++i)
            {
                if (settings.native_format && settings.format_settings && settings.format_settings->native.decode_types_in_binary_format)
                {
                    structure_state->flattened_data_types.push_back(decodeDataType(*structure_stream));
                }
                else
                {
                    readStringBinary(data_type_name, *structure_stream);
                    structure_state->flattened_data_types.push_back(DataTypeFactory::instance().get(data_type_name));
                }
            }

            structure_state->flattened_indexes_type = getIndexesTypeForFlattenedDynamicColumn(num_types);
        }
        else
        {
            if (structure_state->structure_version.value == DynamicSerializationVersion::Value::V1)
            {
                /// Skip max_dynamic_types parameter in V1 serialization version.
                size_t max_dynamic_types;
                readVarUInt(max_dynamic_types, *structure_stream);
            }
            /// Read information about variants.
            DataTypes variants;
            readVarUInt(structure_state->num_dynamic_types, *structure_stream);
            variants.reserve(structure_state->num_dynamic_types + 1); /// +1 for shared variant.
            if (settings.native_format && settings.format_settings && settings.format_settings->native.decode_types_in_binary_format)
            {
                for (size_t i = 0; i != structure_state->num_dynamic_types; ++i)
                    variants.push_back(decodeDataType(*structure_stream));
            }
            else
            {
                String data_type_name;
                for (size_t i = 0; i != structure_state->num_dynamic_types; ++i)
                {
                    readStringBinary(data_type_name, *structure_stream);
                    variants.push_back(DataTypeFactory::instance().get(data_type_name));
                }
            }
            /// Add shared variant, Dynamic column should always have it.
            variants.push_back(ColumnDynamic::getSharedVariantDataType());
            auto variant_type = std::make_shared<DataTypeVariant>(variants);

            /// Read statistics.
            if (settings.object_and_dynamic_read_statistics)
            {
                ColumnDynamic::Statistics statistics(ColumnDynamic::Statistics::Source::READ);
                /// First, read statistics for usual variants.
                for (const auto & variant : variant_type->getVariants())
                    readVarUInt(statistics.variants_statistics[variant->getName()], *structure_stream);

                /// Second, read statistics for shared variants.
                size_t statistics_size;
                readVarUInt(statistics_size, *structure_stream);
                String variant_name;
                for (size_t i = 0; i != statistics_size; ++i)
                {
                    readStringBinary(variant_name, *structure_stream);
                    readVarUInt(statistics.shared_variants_statistics[variant_name], *structure_stream);
                }

                structure_state->statistics = std::make_shared<const ColumnDynamic::Statistics>(std::move(statistics));
            }

            structure_state->variant_type = std::move(variant_type);
        }

        state = structure_state;
        addToSubstreamsDeserializeStatesCache(cache, settings.path, state);
    }

    settings.path.pop_back();
    return state;
}

void SerializationDynamic::serializeBinaryBulkStateSuffix(
    SerializeBinaryBulkSettings & settings, SerializeBinaryBulkStatePtr & state) const
{
    auto * dynamic_state = checkAndGetState<SerializeBinaryBulkStateDynamic>(state);

    if (dynamic_state->structure_version.value == DynamicSerializationVersion::Value::FLATTENED)
    {
        /// Write suffix for indexes and all flattened types.
        settings.path.push_back(Substream::DynamicData);
        auto indexes_serialization = dynamic_state->flattened_column->indexes_type->getDefaultSerialization();
        indexes_serialization->serializeBinaryBulkStateSuffix(settings, dynamic_state->flattened_indexes_state);
        for (size_t i = 0; i != dynamic_state->flattened_column->types.size(); ++i)
        {
            auto serialization = dynamic_state->flattened_column->types[i]->getDefaultSerialization();
            serialization->serializeBinaryBulkStateSuffix(settings, dynamic_state->flattened_states[i]);
        }
        settings.path.pop_back();
        return;
    }

    /// Write statistics in suffix if needed.
    if (settings.object_and_dynamic_write_statistics == SerializeBinaryBulkSettings::ObjectAndDynamicStatisticsMode::SUFFIX)
    {
        settings.path.push_back(Substream::DynamicStructure);
        auto * stream = settings.getter(settings.path);
        settings.path.pop_back();

        if (!stream)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Missing stream for Dynamic column structure during serialization of binary bulk state suffix");

        /// First, write statistics for usual variants.
        for (const auto & variant_name : dynamic_state->variant_names)
            writeVarUInt(dynamic_state->statistics.variants_statistics[variant_name], *stream);
        /// Second, write statistics for shared variants.
        writeVarUInt(dynamic_state->statistics.shared_variants_statistics.size(), *stream);
        for (const auto & [variant_name, size] : dynamic_state->statistics.shared_variants_statistics)
        {
            writeStringBinary(variant_name, *stream);
            writeVarUInt(size, *stream);
        }
    }

    settings.path.push_back(Substream::DynamicData);
    dynamic_state->variant_serialization->serializeBinaryBulkStateSuffix(settings, dynamic_state->variant_state);
    settings.path.pop_back();
}

void SerializationDynamic::serializeBinaryBulkWithMultipleStreams(
    const DB::IColumn & column,
    size_t offset,
    size_t limit,
    SerializeBinaryBulkSettings & settings,
    SerializeBinaryBulkStatePtr & state) const
{
    size_t tmp_size;
    serializeBinaryBulkWithMultipleStreamsAndCountTotalSizeOfVariants(column, offset, limit, settings, state, tmp_size);
}

void SerializationDynamic::serializeBinaryBulkWithMultipleStreamsAndCountTotalSizeOfVariants(
    const IColumn & column,
    size_t offset,
    size_t limit,
    SerializeBinaryBulkSettings & settings,
    SerializeBinaryBulkStatePtr & state,
    size_t & total_size_of_variants) const
{
    const auto & column_dynamic = assert_cast<const ColumnDynamic &>(column);
    auto * dynamic_state = checkAndGetState<SerializeBinaryBulkStateDynamic>(state);
    if (dynamic_state->structure_version.value == DynamicSerializationVersion::Value::FLATTENED)
    {
        if (offset != 0 || (limit != 0 && limit != column.size()))
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Flattened serialization for Dynamic column is supported only when the whole column is serialized. Provided offset and limit: {}/{}", offset, limit);

        settings.path.push_back(Substream::DynamicData);
        /// First, write indexes.
        dynamic_state->flattened_column->indexes_type->getDefaultSerialization()->serializeBinaryBulkWithMultipleStreams(*dynamic_state->flattened_column->indexes_column, 0, 0, settings, dynamic_state->flattened_indexes_state);
        /// Second, write all data of flattened types in corresponding order.
        for (size_t i = 0; i != dynamic_state->flattened_column->types.size(); ++i)
        {
            auto serialization = dynamic_state->flattened_column->types[i]->getDefaultSerialization();
            serialization->serializeBinaryBulkWithMultipleStreams(*dynamic_state->flattened_column->columns[i], 0, 0, settings, dynamic_state->flattened_states[i]);
        }
        settings.path.pop_back();
        return;
    }

    const auto & variant_info = column_dynamic.getVariantInfo();
    const auto * variant_column = &column_dynamic.getVariantColumn();

    if (!variant_info.variant_type->equals(*dynamic_state->variant_type))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Mismatch of internal columns of Dynamic. Expected: {}, Got: {}", dynamic_state->variant_type->getName(), variant_info.variant_type->getName());

    settings.path.push_back(Substream::DynamicData);
    assert_cast<const SerializationVariant &>(*dynamic_state->variant_serialization)
        .serializeBinaryBulkWithMultipleStreamsAndUpdateVariantStatistics(
            *variant_column,
            offset,
            limit,
            settings,
            dynamic_state->variant_state,
            dynamic_state->statistics.variants_statistics,
            total_size_of_variants);

    if (dynamic_state->recalculate_statistics)
    {
        /// Calculate statistics for shared variants.
        const auto & shared_variant = column_dynamic.getSharedVariant();
        if (!shared_variant.empty())
        {
            const auto & local_discriminators = variant_column->getLocalDiscriminators();
            const auto & offsets = variant_column->getOffsets();
            const auto shared_variant_discr = variant_column->localDiscriminatorByGlobal(column_dynamic.getSharedVariantDiscriminator());
            size_t end = limit == 0 || offset + limit > local_discriminators.size() ? local_discriminators.size() : offset + limit;
            for (size_t i = offset; i != end; ++i)
            {
                if (local_discriminators[i] == shared_variant_discr)
                {
                    auto value = shared_variant.getDataAt(offsets[i]);
                    ReadBufferFromMemory buf(value.data, value.size);
                    auto type = decodeDataType(buf);
                    auto type_name = type->getName();
                    if (auto it = dynamic_state->statistics.shared_variants_statistics.find(type_name); it != dynamic_state->statistics.shared_variants_statistics.end())
                        ++it->second;
                    else if (dynamic_state->statistics.shared_variants_statistics.size() < ColumnDynamic::Statistics::MAX_SHARED_VARIANT_STATISTICS_SIZE)
                        dynamic_state->statistics.shared_variants_statistics.emplace(type_name, 1);
                }
            }
        }
    }
    settings.path.pop_back();
}

void SerializationDynamic::deserializeBinaryBulkWithMultipleStreams(
    DB::ColumnPtr & column,
    size_t rows_offset,
    size_t limit,
    DeserializeBinaryBulkSettings & settings,
    DeserializeBinaryBulkStatePtr & state,
    SubstreamsCache * cache) const
{
    if (!state)
        return;

    auto mutable_column = column->assumeMutable();
    auto & column_dynamic = assert_cast<ColumnDynamic &>(*mutable_column);
    auto * dynamic_state = checkAndGetState<DeserializeBinaryBulkStateDynamic>(state);
    auto * structure_state = checkAndGetState<DeserializeBinaryBulkStateDynamicStructure>(dynamic_state->structure_state);

    if (structure_state->structure_version.value == DynamicSerializationVersion::Value::FLATTENED)
    {
        settings.path.push_back(Substream::DynamicData);

        FlattenedDynamicColumn flattened_column;
        flattened_column.types = structure_state->flattened_data_types;
        flattened_column.indexes_type = structure_state->flattened_indexes_type;
        flattened_column.indexes_column = flattened_column.indexes_type->createColumn();
        /// First, read indexes.
        auto indexes_serialization = flattened_column.indexes_type->getDefaultSerialization();
        indexes_serialization->deserializeBinaryBulkWithMultipleStreams(flattened_column.indexes_column, 0, limit, settings, dynamic_state->flattened_indexes_state, cache);
        /// Second, read data of all flattened types in corresponding order.
        auto flattened_limits = getLimitsForFlattenedDynamicColumn(*flattened_column.indexes_column, flattened_column.types.size());
        flattened_column.columns.reserve(flattened_column.types.size());
        for (size_t i = 0; i != flattened_column.types.size(); ++i)
        {
            ColumnPtr type_column = flattened_column.types[i]->createColumn();
            flattened_column.types[i]->getDefaultSerialization()->deserializeBinaryBulkWithMultipleStreams(type_column, 0, flattened_limits[i], settings, dynamic_state->flattened_states[i], cache);
            flattened_column.columns.emplace_back(std::move(type_column));
        }

        settings.path.pop_back();

        unflattenDynamicColumn(std::move(flattened_column), column_dynamic);
        column = std::move(mutable_column);
        return;
    }

    if (mutable_column->empty())
    {
        column_dynamic.setMaxDynamicPaths(structure_state->num_dynamic_types);
        column_dynamic.setVariantType(structure_state->variant_type);
        column_dynamic.setStatistics(structure_state->statistics);
    }

    const auto & variant_info = column_dynamic.getVariantInfo();
    if (!variant_info.variant_type->equals(*structure_state->variant_type))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Mismatch of internal columns of Dynamic. Expected: {}, Got: {}", structure_state->variant_type->getName(), variant_info.variant_type->getName());

    settings.path.push_back(Substream::DynamicData);
    dynamic_state->variant_serialization->deserializeBinaryBulkWithMultipleStreams(column_dynamic.getVariantColumnPtr(), rows_offset, limit, settings, dynamic_state->variant_state, cache);
    settings.path.pop_back();

    column = std::move(mutable_column);
}

void SerializationDynamic::serializeBinary(const Field & field, WriteBuffer & ostr, const FormatSettings & settings) const
{
    /// Serialize NULL as Nothing type with no value.
    if (field.isNull())
    {
        encodeDataType(std::make_shared<DataTypeNothing>(), ostr);
        return;
    }

    auto field_type = applyVisitor(FieldToDataType(), field);
    encodeDataType(field_type, ostr);
    field_type->getDefaultSerialization()->serializeBinary(field, ostr, settings);
}

void SerializationDynamic::deserializeBinary(Field & field, ReadBuffer & istr, const FormatSettings & settings) const
{
    auto field_type = decodeDataType(istr);
    if (isNothing(field_type))
    {
        field = Null();
        return;
    }

    field_type->getDefaultSerialization()->deserializeBinary(field, istr, settings);
}

void SerializationDynamic::serializeBinary(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    const auto & dynamic_column = assert_cast<const ColumnDynamic &>(column);
    const auto & variant_info = dynamic_column.getVariantInfo();
    const auto & variant_column = dynamic_column.getVariantColumn();
    auto global_discr = variant_column.globalDiscriminatorAt(row_num);

    /// Serialize NULL as Nothing type with no value.
    if (global_discr == ColumnVariant::NULL_DISCRIMINATOR)
    {
        encodeDataType(std::make_shared<DataTypeNothing>(), ostr);
        return;
    }
    /// Check if this value is in shared variant. In this case it's already
    /// in desired binary format.
    if (global_discr == dynamic_column.getSharedVariantDiscriminator())
    {
        auto value = dynamic_column.getSharedVariant().getDataAt(variant_column.offsetAt(row_num));
        ostr.write(value.data, value.size);
        return;
    }

    const auto & variant_type = assert_cast<const DataTypeVariant &>(*variant_info.variant_type).getVariant(global_discr);
    encodeDataType(variant_type, ostr);
    variant_type->getDefaultSerialization()->serializeBinary(variant_column.getVariantByGlobalDiscriminator(global_discr), variant_column.offsetAt(row_num), ostr, settings);
}

template <typename ReturnType = void, typename DeserializeFunc>
static ReturnType deserializeVariant(
    ColumnVariant & variant_column,
    const SerializationPtr & variant_serialization,
    ColumnVariant::Discriminator global_discr,
    ReadBuffer & istr,
    DeserializeFunc deserialize)
{
    auto & variant = variant_column.getVariantByGlobalDiscriminator(global_discr);
    if constexpr (std::is_same_v<ReturnType, bool>)
    {
        if (!deserialize(*variant_serialization, variant, istr))
            return ReturnType(false);
    }
    else
    {
        deserialize(*variant_serialization, variant, istr);
    }
    variant_column.getLocalDiscriminators().push_back(variant_column.localDiscriminatorByGlobal(global_discr));
    variant_column.getOffsets().push_back(variant.size() - 1);
    return ReturnType(true);
}

void SerializationDynamic::deserializeBinary(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    auto & dynamic_column = assert_cast<ColumnDynamic &>(column);
    auto variant_type = decodeDataType(istr);
    if (isNothing(variant_type))
    {
        dynamic_column.insertDefault();
        return;
    }

    auto variant_type_name = variant_type->getName();
    const auto & variant_serialization = dynamic_column.getVariantSerialization(variant_type, variant_type_name);
    const auto & variant_info = dynamic_column.getVariantInfo();
    auto it = variant_info.variant_name_to_discriminator.find(variant_type_name);
    if (it != variant_info.variant_name_to_discriminator.end())
    {
        deserializeVariant(dynamic_column.getVariantColumn(), variant_serialization, it->second, istr, [&settings](const ISerialization & serialization, IColumn & variant, ReadBuffer & buf){ serialization.deserializeBinary(variant, buf, settings); });
        return;
    }

    /// We don't have this variant yet. Let's try to add it.
    if (dynamic_column.addNewVariant(variant_type))
    {
        auto discr = variant_info.variant_name_to_discriminator.at(variant_type_name);
        deserializeVariant(dynamic_column.getVariantColumn(), variant_serialization, discr, istr, [&settings](const ISerialization & serialization, IColumn & variant, ReadBuffer & buf){ serialization.deserializeBinary(variant, buf, settings); });
        return;
    }

    /// We reached maximum number of variants and couldn't add new variant.
    /// In this case we insert this value into shared variant in binary form.
    auto tmp_variant_column = variant_type->createColumn();
    variant_serialization->deserializeBinary(*tmp_variant_column, istr, settings);
    dynamic_column.insertValueIntoSharedVariant(*tmp_variant_column, variant_type, variant_type_name, 0);
}

template <typename ReadFieldFunc, typename TryDeserializeVariantFunc, typename DeserializeVariant>
static void deserializeTextImpl(
    IColumn & column,
    ReadBuffer & istr,
    const FormatSettings & settings,
    ReadFieldFunc read_field,
    FormatSettings::EscapingRule escaping_rule,
    TryDeserializeVariantFunc try_deserialize_variant,
    DeserializeVariant deserialize_variant)
{
    auto & dynamic_column = assert_cast<ColumnDynamic &>(column);
    auto & variant_column = dynamic_column.getVariantColumn();
    const auto & variant_info = dynamic_column.getVariantInfo();
    const auto & variant_types = assert_cast<const DataTypeVariant &>(*variant_info.variant_type).getVariants();
    String field = read_field(istr);
    JSONInferenceInfo json_info;
    auto variant_type = tryInferDataTypeByEscapingRule(field, settings, escaping_rule, &json_info);
    if (escaping_rule == FormatSettings::EscapingRule::JSON)
        transformFinalInferredJSONTypeIfNeeded(variant_type, settings, &json_info);

    /// If inferred type is not complete, we cannot add it as a new variant.
    /// Let's try to deserialize this field into existing variants.
    /// If failed, insert this value as String.
    if (!checkIfTypeIsComplete(variant_type))
    {
        size_t shared_variant_discr = dynamic_column.getSharedVariantDiscriminator();
        for (size_t i = 0; i != variant_types.size(); ++i)
        {
            auto field_buf = std::make_unique<ReadBufferFromString>(field);
            if (i != shared_variant_discr
                && deserializeVariant<bool>(
                    variant_column,
                    dynamic_column.getVariantSerialization(variant_types[i], variant_info.variant_names[i]),
                    i,
                    *field_buf,
                    try_deserialize_variant))
                return;
        }

        /// We cannot insert value with incomplete type, insert it as String.
        variant_type = std::make_shared<DataTypeString>();
        /// To be able to deserialize field as String with Quoted escaping rule, it should be quoted.
        if (escaping_rule == FormatSettings::EscapingRule::Quoted && (field.size() < 2 || field.front() != '\'' || field.back() != '\''))
            field = "'" + field + "'";
    }

    if (dynamic_column.addNewVariant(variant_type, variant_type->getName()))
    {
        auto field_buf = std::make_unique<ReadBufferFromString>(field);
        auto discr = variant_info.variant_name_to_discriminator.at(variant_type->getName());
        deserializeVariant(dynamic_column.getVariantColumn(), dynamic_column.getVariantSerialization(variant_type), discr, *field_buf, deserialize_variant);
        return;
    }

    /// We couldn't add new variant. Insert it into shared variant.
    auto tmp_variant_column = variant_type->createColumn();
    auto field_buf = std::make_unique<ReadBufferFromString>(field);
    auto variant_type_name = variant_type->getName();
    deserialize_variant(*dynamic_column.getVariantSerialization(variant_type, variant_type_name), *tmp_variant_column, *field_buf);
    dynamic_column.insertValueIntoSharedVariant(*tmp_variant_column, variant_type, variant_type_name, 0);
}

template <typename NestedSerialize>
static void serializeTextImpl(
    const IColumn & column,
    size_t row_num,
    WriteBuffer & ostr,
    NestedSerialize nested_serialize)
{
    const auto & dynamic_column = assert_cast<const ColumnDynamic &>(column);
    const auto & variant_column = dynamic_column.getVariantColumn();
    /// Check if this row has value in shared variant. In this case we should first deserialize it from binary format.
    if (variant_column.globalDiscriminatorAt(row_num) == dynamic_column.getSharedVariantDiscriminator())
    {
        auto value = dynamic_column.getSharedVariant().getDataAt(variant_column.offsetAt(row_num));
        ReadBufferFromMemory buf(value.data, value.size);
        auto variant_type = decodeDataType(buf);
        auto tmp_variant_column = variant_type->createColumn();
        auto variant_serialization = variant_type->getDefaultSerialization();
        variant_serialization->deserializeBinary(*tmp_variant_column, buf, FormatSettings{});
        nested_serialize(*variant_serialization, *tmp_variant_column, 0, ostr);
    }
    /// Otherwise just use serialization for Variant.
    else
    {
        nested_serialize(*dynamic_column.getVariantInfo().variant_type->getDefaultSerialization(), variant_column, row_num, ostr);
    }
}

void SerializationDynamic::serializeTextCSV(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    auto nested_serialize = [&settings](const ISerialization & serialization, const IColumn & col, size_t row, WriteBuffer & buf)
    {
        serialization.serializeTextCSV(col, row, buf, settings);
    };

    serializeTextImpl(column, row_num, ostr, nested_serialize);
}

void SerializationDynamic::deserializeTextCSV(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    auto read_field = [&settings](ReadBuffer & buf)
    {
        String field;
        readCSVField(field, buf, settings.csv);
        return field;
    };

    auto try_deserialize_variant = [&settings](const ISerialization & serialization, IColumn & col, ReadBuffer & buf)
    {
        return serialization.tryDeserializeTextCSV(col, buf, settings);
    };

    auto deserialize_variant = [&settings](const ISerialization & serialization, IColumn & col, ReadBuffer & buf)
    {
        serialization.deserializeTextCSV(col, buf, settings);
    };

    deserializeTextImpl(column, istr, settings, read_field, FormatSettings::EscapingRule::CSV, try_deserialize_variant, deserialize_variant);
}

bool SerializationDynamic::tryDeserializeTextCSV(DB::IColumn & column, DB::ReadBuffer & istr, const DB::FormatSettings & settings) const
{
    deserializeTextCSV(column, istr, settings);
    return true;
}

void SerializationDynamic::serializeTextEscaped(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    auto nested_serialize = [&settings](const ISerialization & serialization, const IColumn & col, size_t row, WriteBuffer & buf)
    {
        serialization.serializeTextEscaped(col, row, buf, settings);
    };

    serializeTextImpl(column, row_num, ostr, nested_serialize);
}

void SerializationDynamic::deserializeTextEscaped(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    auto read_field = [](ReadBuffer & buf)
    {
        String field;
        readEscapedString(field, buf);
        return field;
    };

    auto try_deserialize_variant = [&settings](const ISerialization & serialization, IColumn & col, ReadBuffer & buf)
    {
        return serialization.tryDeserializeTextEscaped(col, buf, settings);
    };

    auto deserialize_variant = [&settings](const ISerialization & serialization, IColumn & col, ReadBuffer & buf)
    {
        serialization.deserializeTextEscaped(col, buf, settings);
    };

    deserializeTextImpl(column, istr, settings, read_field, FormatSettings::EscapingRule::Escaped, try_deserialize_variant, deserialize_variant);
}

bool SerializationDynamic::tryDeserializeTextEscaped(DB::IColumn & column, DB::ReadBuffer & istr, const DB::FormatSettings & settings) const
{
    deserializeTextEscaped(column, istr, settings);
    return true;
}

void SerializationDynamic::serializeTextQuoted(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    auto nested_serialize = [&settings](const ISerialization & serialization, const IColumn & col, size_t row, WriteBuffer & buf)
    {
        serialization.serializeTextQuoted(col, row, buf, settings);
    };

    serializeTextImpl(column, row_num, ostr, nested_serialize);
}

void SerializationDynamic::deserializeTextQuoted(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    auto read_field = [](ReadBuffer & buf)
    {
        String field;
        readQuotedField(field, buf);
        return field;
    };

    auto try_deserialize_variant = [&settings](const ISerialization & serialization, IColumn & col, ReadBuffer & buf)
    {
        return serialization.tryDeserializeTextQuoted(col, buf, settings);
    };

    auto deserialize_variant = [&settings](const ISerialization & serialization, IColumn & col, ReadBuffer & buf)
    {
        serialization.deserializeTextQuoted(col, buf, settings);
    };

    deserializeTextImpl(column, istr, settings, read_field, FormatSettings::EscapingRule::Quoted, try_deserialize_variant, deserialize_variant);
}

bool SerializationDynamic::tryDeserializeTextQuoted(DB::IColumn & column, DB::ReadBuffer & istr, const DB::FormatSettings & settings) const
{
    deserializeTextQuoted(column, istr, settings);
    return true;
}

void SerializationDynamic::serializeTextJSON(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    auto nested_serialize = [&settings](const ISerialization & serialization, const IColumn & col, size_t row, WriteBuffer & buf)
    {
        serialization.serializeTextJSON(col, row, buf, settings);
    };

    serializeTextImpl(column, row_num, ostr, nested_serialize);
}

void SerializationDynamic::serializeTextJSONPretty(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings, size_t indent) const
{
    auto nested_serialize = [&settings, indent](const ISerialization & serialization, const IColumn & col, size_t row, WriteBuffer & buf)
    {
        serialization.serializeTextJSONPretty(col, row, buf, settings, indent);
    };

    serializeTextImpl(column, row_num, ostr, nested_serialize);
}

void SerializationDynamic::deserializeTextJSON(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    auto read_field = [&settings](ReadBuffer & buf)
    {
        String field;
        readJSONField(field, buf, settings.json);
        return field;
    };

    auto try_deserialize_variant = [&settings](const ISerialization & serialization, IColumn & col, ReadBuffer & buf)
    {
        return serialization.tryDeserializeTextJSON(col, buf, settings);
    };

    auto deserialize_variant = [&settings](const ISerialization & serialization, IColumn & col, ReadBuffer & buf)
    {
        serialization.deserializeTextJSON(col, buf, settings);
    };

    deserializeTextImpl(column, istr, settings, read_field, FormatSettings::EscapingRule::JSON, try_deserialize_variant, deserialize_variant);
}

bool SerializationDynamic::tryDeserializeTextJSON(DB::IColumn & column, DB::ReadBuffer & istr, const DB::FormatSettings & settings) const
{
    deserializeTextJSON(column, istr, settings);
    return true;
}

void SerializationDynamic::serializeTextRaw(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    auto nested_serialize = [&settings](const ISerialization & serialization, const IColumn & col, size_t row, WriteBuffer & buf)
    {
        serialization.serializeTextRaw(col, row, buf, settings);
    };

    serializeTextImpl(column, row_num, ostr, nested_serialize);
}

void SerializationDynamic::deserializeTextRaw(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    auto read_field = [](ReadBuffer & buf)
    {
        String field;
        readString(field, buf);
        return field;
    };

    auto try_deserialize_variant = [&settings](const ISerialization & serialization, IColumn & col, ReadBuffer & buf)
    {
        return serialization.tryDeserializeTextRaw(col, buf, settings);
    };

    auto deserialize_variant = [&settings](const ISerialization & serialization, IColumn & col, ReadBuffer & buf)
    {
        serialization.deserializeTextRaw(col, buf, settings);
    };

    deserializeTextImpl(column, istr, settings, read_field, FormatSettings::EscapingRule::Raw, try_deserialize_variant, deserialize_variant);
}

bool SerializationDynamic::tryDeserializeTextRaw(DB::IColumn & column, DB::ReadBuffer & istr, const DB::FormatSettings & settings) const
{
    deserializeTextRaw(column, istr, settings);
    return true;
}

void SerializationDynamic::serializeText(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    auto nested_serialize = [&settings](const ISerialization & serialization, const IColumn & col, size_t row, WriteBuffer & buf)
    {
        serialization.serializeText(col, row, buf, settings);
    };

    serializeTextImpl(column, row_num, ostr, nested_serialize);
}

void SerializationDynamic::deserializeWholeText(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    auto read_field = [](ReadBuffer & buf)
    {
        String field;
        readStringUntilEOF(field, buf);
        return field;
    };

    auto try_deserialize_variant = [&settings](const ISerialization & serialization, IColumn & col, ReadBuffer & buf)
    {
        return serialization.tryDeserializeWholeText(col, buf, settings);
    };

    auto deserialize_variant = [&settings](const ISerialization & serialization, IColumn & col, ReadBuffer & buf)
    {
        serialization.deserializeWholeText(col, buf, settings);
    };

    deserializeTextImpl(column, istr, settings, read_field, FormatSettings::EscapingRule::Raw, try_deserialize_variant, deserialize_variant);
}

bool SerializationDynamic::tryDeserializeWholeText(DB::IColumn & column, DB::ReadBuffer & istr, const DB::FormatSettings & settings) const
{
    deserializeWholeText(column, istr, settings);
    return true;
}

void SerializationDynamic::serializeTextXML(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    auto nested_serialize = [&settings](const ISerialization & serialization, const IColumn & col, size_t row, WriteBuffer & buf)
    {
        serialization.serializeTextXML(col, row, buf, settings);
    };

    serializeTextImpl(column, row_num, ostr, nested_serialize);
}

}
