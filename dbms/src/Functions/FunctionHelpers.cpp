#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnLowCardinality.h>
#include <Common/assert_cast.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <IO/WriteHelpers.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_COLUMN;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int SIZES_OF_ARRAYS_DOESNT_MATCH;
}

const ColumnConst * checkAndGetColumnConstStringOrFixedString(const IColumn * column)
{
    if (!isColumnConst(*column))
        return {};

    const ColumnConst * res = assert_cast<const ColumnConst *>(column);

    if (checkColumn<ColumnString>(&res->getDataColumn())
        || checkColumn<ColumnFixedString>(&res->getDataColumn()))
        return res;

    return {};
}


Columns convertConstTupleToConstantElements(const ColumnConst & column)
{
    const ColumnTuple & src_tuple = assert_cast<const ColumnTuple &>(column.getDataColumn());
    const auto & src_tuple_columns = src_tuple.getColumns();
    size_t tuple_size = src_tuple_columns.size();
    size_t rows = column.size();

    Columns res(tuple_size);
    for (size_t i = 0; i < tuple_size; ++i)
        res[i] = ColumnConst::create(src_tuple_columns[i], rows);

    return res;
}


static Block createBlockWithNestedColumnsImpl(const Block & block, const std::unordered_set<size_t> & args)
{
    Block res;
    size_t columns = block.columns();

    for (size_t i = 0; i < columns; ++i)
    {
        const auto & col = block.getByPosition(i);

        if (args.count(i) && col.type->isNullable())
        {
            const DataTypePtr & nested_type = static_cast<const DataTypeNullable &>(*col.type).getNestedType();

            if (!col.column)
            {
                res.insert({nullptr, nested_type, col.name});
            }
            else if (auto * nullable = checkAndGetColumn<ColumnNullable>(*col.column))
            {
                const auto & nested_col = nullable->getNestedColumnPtr();
                res.insert({nested_col, nested_type, col.name});
            }
            else if (auto * const_column = checkAndGetColumn<ColumnConst>(*col.column))
            {
                const auto & nested_col = checkAndGetColumn<ColumnNullable>(const_column->getDataColumn())->getNestedColumnPtr();
                res.insert({ ColumnConst::create(nested_col, col.column->size()), nested_type, col.name});
            }
            else
                throw Exception("Illegal column for DataTypeNullable", ErrorCodes::ILLEGAL_COLUMN);
        }
        else
            res.insert(col);
    }

    return res;
}


Block createBlockWithNestedColumns(const Block & block, const ColumnNumbers & args)
{
    std::unordered_set<size_t> args_set(args.begin(), args.end());
    return createBlockWithNestedColumnsImpl(block, args_set);
}

Block createBlockWithNestedColumns(const Block & block, const ColumnNumbers & args, size_t result)
{
    std::unordered_set<size_t> args_set(args.begin(), args.end());
    args_set.insert(result);
    return createBlockWithNestedColumnsImpl(block, args_set);
}

void validateArgumentType(const IFunction & func, const DataTypes & arguments,
                                 size_t argument_index, bool (* validator_func)(const IDataType &),
                                 const char * expected_type_description)
{
    if (arguments.size() <= argument_index)
        throw Exception("Incorrect number of arguments of function " + func.getName(),
                        ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

    const auto & argument = arguments[argument_index];
    if (validator_func(*argument) == false)
        throw Exception("Illegal type " + argument->getName() +
                        " of " + std::to_string(argument_index) +
                        " argument of function " + func.getName() +
                        " expected " + expected_type_description,
                        ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
}

std::pair<std::vector<const IColumn *>, const ColumnArray::Offset *>
checkAndGetNestedArrayOffset(const IColumn ** columns, size_t num_arguments)
{
    assert(num_arguments > 0);
    std::vector<const IColumn *> nested_columns(num_arguments);
    const ColumnArray::Offsets * offsets = nullptr;
    for (size_t i = 0; i < num_arguments; ++i)
    {
        const ColumnArray::Offsets * offsets_i = nullptr;
        if (const ColumnArray * arr = checkAndGetColumn<const ColumnArray>(columns[i]))
        {
            nested_columns[i] = &arr->getData();
            offsets_i = &arr->getOffsets();
        }
        else
            throw Exception("Illegal column " + columns[i]->getName() + " as argument of function", ErrorCodes::ILLEGAL_COLUMN);
        if (i == 0)
            offsets = offsets_i;
        else if (*offsets_i != *offsets)
            throw Exception("Lengths of all arrays passed to aggregate function must be equal.", ErrorCodes::SIZES_OF_ARRAYS_DOESNT_MATCH);
    }
    return {nested_columns, offsets->data()};
}

}
