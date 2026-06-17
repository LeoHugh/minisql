#include "record/column.h"

#include "glog/logging.h"

Column::Column(std::string column_name, TypeId type, uint32_t index, bool nullable, bool unique)
    : name_(std::move(column_name)), type_(type), table_ind_(index), nullable_(nullable), unique_(unique) {
  ASSERT(type != TypeId::kTypeChar, "Wrong constructor for CHAR type.");
  switch (type) {
    case TypeId::kTypeInt:
      len_ = sizeof(int32_t);
      break;
    case TypeId::kTypeFloat:
      len_ = sizeof(float_t);
      break;
    default:
      ASSERT(false, "Unsupported column type.");
  }
}

Column::Column(std::string column_name, TypeId type, uint32_t length, uint32_t index, bool nullable, bool unique)
    : name_(std::move(column_name)),
      type_(type),
      len_(length),
      table_ind_(index),
      nullable_(nullable),
      unique_(unique) {
  ASSERT(type == TypeId::kTypeChar, "Wrong constructor for non-VARCHAR type.");
}

Column::Column(const Column *other)
    : name_(other->name_),
      type_(other->type_),
      len_(other->len_),
      table_ind_(other->table_ind_),
      nullable_(other->nullable_),
      unique_(other->unique_) {}

/**
* TODO: Student Implement
*/
uint32_t Column::SerializeTo(char *buf) const {
  uint32_t offset = 0;
  MACH_WRITE_UINT32(buf, COLUMN_MAGIC_NUM);
  offset += sizeof(uint32_t);
  MACH_WRITE_UINT32(buf + offset, name_.length());
  offset += sizeof(uint32_t);
  MACH_WRITE_STRING(buf + offset, name_);
  offset += name_.length();
  MACH_WRITE_TO(TypeId, buf + offset, type_);
  offset += sizeof(TypeId);
  MACH_WRITE_UINT32(buf + offset, len_);
  offset += sizeof(uint32_t);
  MACH_WRITE_UINT32(buf + offset, table_ind_);
  offset += sizeof(uint32_t);
  MACH_WRITE_TO(bool, buf + offset, nullable_);
  offset += sizeof(bool);
  MACH_WRITE_TO(bool, buf + offset, unique_);
  offset += sizeof(bool);
  return offset;
}

/**
 * TODO: Student Implement
 */
uint32_t Column::GetSerializedSize() const {
  uint32_t size = 0;
  size += sizeof(uint32_t); 
  size += MACH_STR_SERIALIZED_SIZE(name_); 
  size += sizeof(TypeId); 
  size += sizeof(uint32_t); 
  size += sizeof(uint32_t);
  size += sizeof(bool); 
  size += sizeof(bool); 
  return size;
}

/**
 * TODO: Student Implement
 */
uint32_t Column::DeserializeFrom(char *buf, Column *&column) {
  uint32_t offset = 0;
  uint32_t magic_num = MACH_READ_UINT32(buf);
  ASSERT(magic_num == COLUMN_MAGIC_NUM, "Invalid magic number for Column deserialization.");
  offset += sizeof(uint32_t);
  uint32_t name_len = MACH_READ_UINT32(buf + offset);
  offset += sizeof(uint32_t);
  std::string name(buf + offset, name_len);
  offset += name_len;
  TypeId type = MACH_READ_FROM(TypeId, buf + offset);
  offset += sizeof(TypeId);
  uint32_t len = MACH_READ_UINT32(buf + offset);
  offset += sizeof(uint32_t);
  uint32_t table_ind = MACH_READ_UINT32(buf + offset);
  offset += sizeof(uint32_t);
  bool nullable = MACH_READ_FROM(bool, buf + offset);
  offset += sizeof(bool);
  bool unique = MACH_READ_FROM(bool, buf + offset);
  offset += sizeof(bool);
  if (type == TypeId::kTypeChar) {
    column = new Column(name, type, len, table_ind, nullable, unique);
  } else {
    column = new Column(name, type, table_ind, nullable, unique);
  }
  return offset;
}
