/*******************************************************************\

Module: Symbol Analyzer

Author: Malte Mues <mail.mues@gmail.com>
        Daniel Poetzl

\*******************************************************************/

#include "analyze_symbol.h"

#include <util/c_types.h>
#include <util/c_types_util.h>
#include <util/config.h>
#include <util/expr_initializer.h>
#include <util/pointer_offset_size.h>
#include <util/string2int.h>
#include <util/string_constant.h>
#include <util/string_utils.h>

gdb_value_extractort::gdb_value_extractort(
  const symbol_tablet &symbol_table,
  const char *binary)
  : gdb_api(binary),
    symbol_table(symbol_table),
    ns(symbol_table),
    c_converter(ns, expr2c_configurationt::clean_configuration),
    allocate_objects(ID_C, source_locationt(), irep_idt{}, this->symbol_table)
{
}

void gdb_value_extractort::analyze_symbols(const std::vector<irep_idt> &symbols)
{
  // record addresses of given symbols
  for(const auto &id : symbols)
  {
    const symbol_exprt &symbol_expr = ns.lookup(id).symbol_expr();
    const address_of_exprt aoe(symbol_expr);

    const std::string c_expr = c_converter.convert(aoe);
    const pointer_valuet &value = gdb_api.get_memory(c_expr);
    CHECK_RETURN(value.pointee.empty() || (id == value.pointee));

    values.insert({value.address, symbol_expr});
  }

  for(const auto &id : symbols)
  {
    analyze_symbol(id);
  }
}

void gdb_value_extractort::analyze_symbol(const irep_idt &symbol_name)
{
  const symbolt &symbol = ns.lookup(symbol_name);
  const symbol_exprt symbol_expr = symbol.symbol_expr();

  try
  {
    const typet target_type = symbol.type;

    const auto zero_expr = zero_initializer(target_type, symbol.location, ns);
    CHECK_RETURN(zero_expr);

    const exprt target_expr =
      get_expr_value(symbol_expr, *zero_expr, symbol.location);

    add_assignment(symbol_expr, target_expr);
  }
  catch(const gdb_interaction_exceptiont &e)
  {
    throw analysis_exceptiont(e.what());
  }

  process_outstanding_assignments();
}

/// Get memory snapshot as C code
std::string gdb_value_extractort::get_snapshot_as_c_code()
{
  code_blockt generated_code;

  allocate_objects.declare_created_symbols(generated_code);

  for(auto const &pair : assignments)
  {
    generated_code.add(code_assignt(pair.first, pair.second));
  }

  return c_converter.convert(generated_code);
}

/// Get memory snapshot as symbol table
symbol_tablet gdb_value_extractort::get_snapshot_as_symbol_table()
{
  symbol_tablet snapshot;

  for(const auto &pair : assignments)
  {
    const symbol_exprt &symbol_expr = to_symbol_expr(pair.first);
    const irep_idt id = symbol_expr.get_identifier();

    INVARIANT(symbol_table.has_symbol(id), "symbol must exist in symbol table");

    const symbolt &symbol = symbol_table.lookup_ref(id);

    symbolt snapshot_symbol(symbol);
    snapshot_symbol.value = pair.second;

    snapshot.insert(snapshot_symbol);
  }

  // Also add type symbols to the snapshot
  for(const auto &pair : symbol_table)
  {
    const symbolt &symbol = pair.second;

    if(symbol.is_type)
    {
      snapshot.insert(symbol);
    }
  }

  return snapshot;
}

void gdb_value_extractort::add_assignment(const exprt &lhs, const exprt &value)
{
  assignments.push_back(std::make_pair(lhs, value));
}

exprt gdb_value_extractort::get_char_pointer_value(
  const exprt &expr,
  const memory_addresst &memory_location,
  const source_locationt &location)
{
  PRECONDITION(expr.type().id() == ID_pointer);
  PRECONDITION(is_c_char_type(expr.type().subtype()));
  PRECONDITION(!memory_location.is_null());

  auto it = values.find(memory_location);

  if(it == values.end())
  {
    std::string c_expr = c_converter.convert(expr);
    pointer_valuet value = gdb_api.get_memory(c_expr);
    CHECK_RETURN(value.string);

    string_constantt init(*value.string);
    CHECK_RETURN(to_array_type(init.type()).is_complete());

    symbol_exprt dummy("tmp", pointer_type(init.type()));
    code_blockt assignments;

    const symbol_exprt new_symbol =
      to_symbol_expr(allocate_objects.allocate_automatic_local_object(
        assignments, dummy, init.type()));

    add_assignment(new_symbol, init);

    values.insert(std::make_pair(memory_location, new_symbol));

    // check that we are returning objects of the right type
    CHECK_RETURN(new_symbol.type().subtype() == expr.type().subtype());
    return new_symbol;
  }
  else
  {
    CHECK_RETURN(it->second.type().subtype() == expr.type().subtype());
    return it->second;
  }
}

exprt gdb_value_extractort::get_pointer_to_member_value(
  const exprt &expr,
  const pointer_valuet &pointer_value,
  const source_locationt &location)
{
  PRECONDITION(expr.type().id() == ID_pointer);
  const auto &memory_location = pointer_value.address;
  std::string memory_location_string = memory_location.string();
  PRECONDITION(memory_location_string != "0x0");
  PRECONDITION(!pointer_value.pointee.empty());

  std::string struct_name;
  size_t member_offset;
  if(pointer_value.has_known_offset())
  {
    std::string member_offset_string;
    split_string(
      pointer_value.pointee, '+', struct_name, member_offset_string, true);
    member_offset = safe_string2size_t(member_offset_string);
  }
  else
  {
    struct_name = pointer_value.pointee;
    member_offset = 0;
  }

  const symbolt *struct_symbol = symbol_table.lookup(struct_name);
  DATA_INVARIANT(struct_symbol != nullptr, "unknown struct");
  const auto maybe_struct_size =
    pointer_offset_size(struct_symbol->symbol_expr().type(), ns);
  bool found = false;
  CHECK_RETURN(maybe_struct_size.has_value());
  for(const auto &value_pair : values)
  {
    const auto &value_symbol_expr = value_pair.second;
    if(to_symbol_expr(value_symbol_expr).get_identifier() == struct_name)
    {
      found = true;
      break;
    }
  }

  if(!found)
  {
    const typet target_type = expr.type().subtype();

    symbol_exprt dummy("tmp", expr.type());
    code_blockt assignments;

    auto emplace_pair = values.emplace(
      memory_location,
      allocate_objects.allocate_automatic_local_object(
        assignments, dummy, target_type));
    const symbol_exprt &new_symbol = to_symbol_expr(emplace_pair.first->second);

    dereference_exprt dereference_expr(expr);

    const auto zero_expr = zero_initializer(target_type, location, ns);
    CHECK_RETURN(zero_expr);

    // add assignment of value to newly created symbol
    add_assignment(new_symbol, *zero_expr);

    const auto &struct_symbol = values.find(memory_location);

    const auto maybe_member_expr = get_subexpression_at_offset(
      struct_symbol->second, member_offset, expr.type().subtype(), ns);
    CHECK_RETURN(maybe_member_expr.has_value());
    return *maybe_member_expr;
  }

  const auto it = values.find(memory_location);
  // if the structure we are pointing to does not exists we need to build a
  // temporary object for it: get the type from symbol table, query gdb for
  // value, allocate new object for it and then store into assignments
  if(it == values.end())
  {
    const auto symbol_expr = struct_symbol->symbol_expr();
    const auto zero = zero_initializer(symbol_expr.type(), location, ns);
    CHECK_RETURN(zero.has_value());
    const auto val = get_expr_value(symbol_expr, *zero, location);

    symbol_exprt dummy("tmp", pointer_type(symbol_expr.type()));
    code_blockt assignments;

    const symbol_exprt new_symbol =
      to_symbol_expr(allocate_objects.allocate_automatic_local_object(
        assignments, dummy, symbol_expr.type()));

    add_assignment(new_symbol, val);
    values[memory_location] = val;
  }

  const auto maybe_member_expr = get_subexpression_at_offset(
    struct_symbol->symbol_expr(), member_offset, expr.type().subtype(), ns);
  DATA_INVARIANT(
    maybe_member_expr.has_value(), "structure doesn't have member");

  // check that we return the right type
  CHECK_RETURN(maybe_member_expr->type() == expr.type().subtype());
  return *maybe_member_expr;
}

exprt gdb_value_extractort::get_non_char_pointer_value(
  const exprt &expr,
  const memory_addresst &memory_location,
  const source_locationt &location)
{
  PRECONDITION(expr.type().id() == ID_pointer);
  PRECONDITION(!is_c_char_type(expr.type().subtype()));
  PRECONDITION(!memory_location.is_null());

  auto it = values.find(memory_location);

  if(it == values.end())
  {
    values.insert(std::make_pair(memory_location, nil_exprt()));

    const typet target_type = expr.type().subtype();

    symbol_exprt dummy("tmp", expr.type());
    code_blockt assignments;

    const auto zero_expr = zero_initializer(target_type, location, ns);
    CHECK_RETURN(zero_expr);

    // Check if pointer was dynamically allocated (via malloc). If so we will
    // replace the pointee with a static array filled with values stored at the
    // expected positions. Since the allocated size is over-approximation we may
    // end up querying pass the allocated bounds and building larger array with
    // meaningless values.
    size_t allocated_size =
      gdb_api.query_malloc_size(c_converter.convert(expr));
    // get the sizeof(target_type) and thus the number of elements
    const auto target_size_bits = pointer_offset_bits(target_type, ns);
    CHECK_RETURN(target_size_bits.has_value());
    const auto number_of_elements = allocated_size / (*target_size_bits / 8);
    if(number_of_elements > 1)
    {
      array_exprt::operandst elements;
      // build the operands by querying for an index expression
      for(size_t i = 0; i < number_of_elements; i++)
      {
        const auto sub_expr_value = get_expr_value(
          index_exprt{expr, from_integer(i, index_type())},
          *zero_expr,
          location);
        elements.push_back(sub_expr_value);
      }
      CHECK_RETURN(elements.size() == number_of_elements);

      // knowing the number of elements we can build the type
      const typet target_array_type =
        array_typet{target_type, from_integer(elements.size(), index_type())};

      array_exprt new_array{elements, to_array_type(target_array_type)};

      // allocate a new symbol for the temporary static array
      symbol_exprt array_dummy("tmp", pointer_type(target_array_type));
      const auto array_symbol =
        allocate_objects.allocate_automatic_local_object(
          assignments, array_dummy, target_array_type);

      // add assignment of value to newly created symbol
      add_assignment(array_symbol, new_array);
      values[memory_location] = array_symbol;
      return array_symbol;
    }

    const symbol_exprt new_symbol =
      to_symbol_expr(allocate_objects.allocate_automatic_local_object(
        assignments, dummy, target_type));

    dereference_exprt dereference_expr(expr);

    const exprt target_expr =
      get_expr_value(dereference_expr, *zero_expr, location);
    // add assignment of value to newly created symbol
    add_assignment(new_symbol, target_expr);

    values[memory_location] = new_symbol;

    return new_symbol;
  }
  else
  {
    const auto &known_value = it->second;
    const auto &expected_type = expr.type().subtype();
    if(known_value.is_not_nil() && known_value.type() != expected_type)
    {
      return symbol_exprt{to_symbol_expr(known_value).get_identifier(),
                          expected_type};
    }
    return known_value;
  }
}

bool gdb_value_extractort::points_to_member(
  const pointer_valuet &pointer_value) const
{
  if(pointer_value.has_known_offset())
    return true;

  const symbolt *pointee_symbol = symbol_table.lookup(pointer_value.pointee);
  if(pointee_symbol == nullptr)
    return false;
  const auto &pointee_type = pointee_symbol->type;
  return pointee_type.id() == ID_struct_tag ||
         pointee_type.id() == ID_union_tag || pointee_type.id() == ID_array ||
         pointee_type.id() == ID_struct || pointee_type.id() == ID_union;
}

exprt gdb_value_extractort::get_pointer_value(
  const exprt &expr,
  const exprt &zero_expr,
  const source_locationt &location)
{
  PRECONDITION(zero_expr.id() == ID_constant);

  PRECONDITION(expr.type().id() == ID_pointer);
  PRECONDITION(expr.type() == zero_expr.type());

  std::string c_expr = c_converter.convert(expr);
  const pointer_valuet value = gdb_api.get_memory(c_expr);
  if(!value.valid)
    return zero_expr;

  const auto memory_location = value.address;

  if(!memory_location.is_null())
  {
    // pointers-to-char can point to members as well, e.g. char[]
    if(points_to_member(value))
    {
      const auto target_expr =
        get_pointer_to_member_value(expr, value, location);
      CHECK_RETURN(target_expr.is_not_nil());
      const auto result_expr = address_of_exprt(target_expr);
      CHECK_RETURN(result_expr.type() == zero_expr.type());
      return result_expr;
    }

    // non-member: split for char/non-char
    const auto target_expr =
      is_c_char_type(expr.type().subtype())
        ? get_char_pointer_value(expr, memory_location, location)
        : get_non_char_pointer_value(expr, memory_location, location);

    // postpone if we cannot resolve now
    if(target_expr.is_nil())
    {
      outstanding_assignments[expr] = memory_location;
      return zero_expr;
    }

    // the pointee was (probably) dynamically allocated (but the allocation
    // would not be visible in the snapshot) so we pretend it is statically
    // allocated (we have the value) and return address to the first element
    // of the array (instead of the array as char*)
    if(target_expr.type().id() == ID_array)
    {
      const auto result_indexed_expr = get_subexpression_at_offset(
        target_expr, 0, zero_expr.type().subtype(), ns);
      CHECK_RETURN(result_indexed_expr.has_value());
      const auto result_expr = address_of_exprt{*result_indexed_expr};
      return result_expr;
    }

    // if the types match return right away
    if(target_expr.type() == zero_expr.type())
      return target_expr;

    // otherwise the address of target should type-match
    const auto result_expr = address_of_exprt(target_expr);
    CHECK_RETURN(result_expr.type() == zero_expr.type());
    return result_expr;
  }

  return zero_expr;
}

exprt gdb_value_extractort::get_array_value(
  const exprt &expr,
  const exprt &array,
  const source_locationt &location)
{
  PRECONDITION(array.id() == ID_array);

  PRECONDITION(expr.type().id() == ID_array);
  PRECONDITION(expr.type() == array.type());

  exprt new_array(array);

  for(size_t i = 0; i < new_array.operands().size(); ++i)
  {
    const index_exprt index_expr(expr, from_integer(i, index_type()));

    exprt &operand = new_array.operands()[i];

    operand = get_expr_value(index_expr, operand, location);
  }

  return new_array;
}

exprt gdb_value_extractort::get_expr_value(
  const exprt &expr,
  const exprt &zero_expr,
  const source_locationt &location)
{
  PRECONDITION(expr.type() == zero_expr.type());

  const typet &type = expr.type();
  PRECONDITION(type.id() != ID_struct);

  if(is_c_integral_type(type))
  {
    INVARIANT(zero_expr.is_constant(), "zero initializer is a constant");

    std::string c_expr = c_converter.convert(expr);
    const auto maybe_value = gdb_api.get_value(c_expr);
    if(!maybe_value.has_value())
      return zero_expr;
    const std::string value = *maybe_value;

    const mp_integer int_rep = string2integer(value);

    return from_integer(int_rep, type);
  }
  else if(is_c_char_type(type))
  {
    INVARIANT(zero_expr.is_constant(), "zero initializer is a constant");

    // check the char-value and return as bitvector-type value
    std::string c_expr = c_converter.convert(expr);
    const auto maybe_value = gdb_api.get_value(c_expr);
    if(!maybe_value.has_value() || maybe_value->empty())
      return zero_expr;
    const std::string value = *maybe_value;

    const mp_integer int_rep = value[0];
    return from_integer(int_rep, type);
  }
  else if(type.id() == ID_c_bool)
  {
    INVARIANT(zero_expr.is_constant(), "zero initializer is a constant");

    std::string c_expr = c_converter.convert(expr);
    const auto maybe_value = gdb_api.get_value(c_expr);
    if(!maybe_value.has_value())
      return zero_expr;
    const std::string value = *maybe_value;

    return from_c_boolean_value(id2boolean(value), type);
  }
  else if(type.id() == ID_c_enum)
  {
    INVARIANT(zero_expr.is_constant(), "zero initializer is a constant");

    std::string c_expr = c_converter.convert(expr);
    const auto maybe_value = gdb_api.get_value(c_expr);
    if(!maybe_value.has_value())
      return zero_expr;
    const std::string value = *maybe_value;

    return convert_member_name_to_enum_value(value, to_c_enum_type(type));
  }
  else if(type.id() == ID_struct_tag)
  {
    return get_struct_value(expr, zero_expr, location);
  }
  else if(type.id() == ID_array)
  {
    return get_array_value(expr, zero_expr, location);
  }
  else if(type.id() == ID_pointer)
  {
    INVARIANT(zero_expr.is_constant(), "zero initializer is a constant");

    return get_pointer_value(expr, zero_expr, location);
  }
  else if(type.id() == ID_union_tag)
  {
    return get_union_value(expr, zero_expr, location);
  }
  UNREACHABLE;
}

exprt gdb_value_extractort::get_struct_value(
  const exprt &expr,
  const exprt &zero_expr,
  const source_locationt &location)
{
  PRECONDITION(zero_expr.id() == ID_struct);

  PRECONDITION(expr.type().id() == ID_struct_tag);
  PRECONDITION(expr.type() == zero_expr.type());

  exprt new_expr(zero_expr);

  const struct_tag_typet &struct_tag_type = to_struct_tag_type(expr.type());
  const struct_typet &struct_type = ns.follow_tag(struct_tag_type);

  for(size_t i = 0; i < new_expr.operands().size(); ++i)
  {
    const struct_typet::componentt &component = struct_type.components()[i];

    if(component.get_is_padding() || component.type().id() == ID_code)
    {
      continue;
    }

    exprt &operand = new_expr.operands()[i];
    member_exprt member_expr(expr, component);

    operand = get_expr_value(member_expr, operand, location);
  }

  return new_expr;
}

exprt gdb_value_extractort::get_union_value(
  const exprt &expr,
  const exprt &zero_expr,
  const source_locationt &location)
{
  PRECONDITION(zero_expr.id() == ID_union);

  PRECONDITION(expr.type().id() == ID_union_tag);
  PRECONDITION(expr.type() == zero_expr.type());

  exprt new_expr(zero_expr);

  const union_tag_typet &union_tag_type = to_union_tag_type(expr.type());
  const union_typet &union_type = ns.follow_tag(union_tag_type);

  CHECK_RETURN(new_expr.operands().size() == 1);
  const union_typet::componentt &component = union_type.components()[0];
  auto &operand = new_expr.operands()[0];
  operand = get_expr_value(member_exprt{expr, component}, operand, location);
  return new_expr;
}

void gdb_value_extractort::process_outstanding_assignments()
{
  for(const auto &pair : outstanding_assignments)
  {
    const address_of_exprt aoe(values[pair.second]);
    add_assignment(pair.first, aoe);
  }
}

std::string gdb_value_extractort::get_gdb_value(const exprt &expr)
{
  std::string c_expr = c_converter.convert(expr);
  const auto maybe_value = gdb_api.get_value(c_expr);
  CHECK_RETURN(maybe_value.has_value());
  return *maybe_value;
}
