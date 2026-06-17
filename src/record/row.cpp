#include "record/row.h"

/**
 * TODO: Student Implement
 */
uint32_t Row::SerializeTo(char *buf, Schema *schema) const {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");
  uint32_t offset = 0;
  //fields的nums
  MACH_WRITE_UINT32(buf,fields_.size());
  offset += sizeof(uint32_t);
  //位图来避免null
  uint32_t bitmap_size = (fields_.size() + 7) /8;
  std::vector<char> bitmap(bitmap_size,0);
  for (size_t i = 0; i < fields_.size(); ++i)
  {
    if (fields_[i]->IsNull())
    {
      bitmap[i/8] |= (1 << (i%8));
    }
  }
  memcpy(buf + offset, bitmap.data(), bitmap_size);
  offset += bitmap_size;
  //字段数据，非null才写入
  for (size_t i = 0; i < fields_.size(); ++i)
  {
    if (!fields_[i]->IsNull())
    {
      offset += fields_[i]->SerializeTo(buf + offset);
    }
  }
  return offset;
}

uint32_t Row::DeserializeFrom(char *buf, Schema *schema) {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(fields_.empty(), "Non empty field in row.");
  uint32_t offset = 0;
  uint32_t field_count = MACH_READ_UINT32(buf);
  offset += sizeof(uint32_t);
  std::vector<char> bitmap((field_count + 7) /8,0);
  memcpy(bitmap.data(), buf + offset, bitmap.size());
  offset += bitmap.size();
  for (size_t i = 0; i < field_count; ++i)
  {
    bool is_null = (bitmap[i/8] & (1 << (i%8))) != 0;
    Field *field;
    offset += Field::DeserializeFrom(buf + offset, schema->GetColumn(i)->GetType(), &field, is_null);
    fields_.push_back(field);
  }
  return offset;
}

uint32_t Row::GetSerializedSize(Schema *schema) const {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");
  uint32_t size= sizeof(uint32_t) + (fields_.size() + 7) /8;
  for (size_t i = 0; i < fields_.size(); ++i)
  {
    if (!fields_[i]->IsNull())
    {
      size += fields_[i]->GetSerializedSize();
    }
  }
  return size;
}

void Row::GetKeyFromRow(const Schema *schema, const Schema *key_schema, Row &key_row) {
  auto columns = key_schema->GetColumns();
  std::vector<Field> fields;
  uint32_t idx;
  for (auto column : columns) {
    schema->GetColumnIndex(column->GetName(), idx);
    fields.emplace_back(*this->GetField(idx));
  }
  key_row = Row(fields);
}
