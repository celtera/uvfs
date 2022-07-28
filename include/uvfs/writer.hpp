#pragma once
#include "config.hpp"

#include <memory>

#include <string_view>

namespace uvfs
{
struct UVFS_EXPORT writer
{
public:
  explicit writer();
  writer(const writer&) = delete;
  writer(writer&&) noexcept = delete;
  auto operator=(const writer&) -> writer& = delete;
  auto operator=(writer&&) noexcept -> writer& = delete;
  ~writer();

  void add_file(std::string_view path_in_archive, std::string_view path_in_system);
  void commit(std::string_view path);

private:
  struct impl;
  std::unique_ptr<impl> impl;
};
}
