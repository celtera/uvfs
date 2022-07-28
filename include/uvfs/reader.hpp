#pragma once
#include "config.hpp"

#include <functional>
#include <memory>
#include <optional>

#include <string_view>

namespace uvfs
{

struct UVFS_EXPORT reader
{
public:
  explicit reader(std::string_view path);
  reader(const reader&) = delete;
  reader(reader&&) noexcept = delete;
  auto operator=(const reader&) -> reader& = delete;
  auto operator=(reader&&) noexcept -> reader& = delete;
  ~reader();

  using byte_array = std::string_view;

  [[nodiscard]] auto size() const noexcept -> std::size_t;
  [[nodiscard]] auto find(std::string_view path) const noexcept
      -> std::optional<byte_array>;

  struct iter_entry
  {
    std::string_view path;
    byte_array data;
  };

  void for_each_file(const std::function<bool(iter_entry)>&) const;

private:
  struct impl;
  std::unique_ptr<struct impl> impl;
};

}
