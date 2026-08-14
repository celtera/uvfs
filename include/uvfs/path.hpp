#pragma once
#include "config.hpp"

#include <cstdint>
#include <string_view>

namespace uvfs
{

// Archive paths
// ------------------------------------------------------------------------
// A path inside a uvfs archive is a UTF-8 byte string using '/' as the only
// separator. It is a lookup key, not a filesystem path: uvfs itself never
// resolves it. It is validated at write time anyway, because anything that
// extracts an archive to disk will join these names onto an output directory,
// and a name containing ".." escapes that directory.
//
// A leading '/' is allowed and is kept verbatim -- "/a" and "a" are different
// keys. Anything extracting to disk must strip leading separators.

//! Largest archive path uvfs will store, in bytes.
inline constexpr int64_t max_archive_path_size = 65535;

enum class path_problem
{
  ok,
  empty,
  too_long,
  contains_nul,
  contains_backslash,
  empty_component,
  dot_component,
  dotdot_component,
};

[[nodiscard]] constexpr auto describe(path_problem p) noexcept -> const char*
{
  switch (p)
  {
    case path_problem::ok:
      return "ok";
    case path_problem::empty:
      return "path is empty";
    case path_problem::too_long:
      return "path is longer than 65535 bytes";
    case path_problem::contains_nul:
      return "path contains a NUL byte";
    case path_problem::contains_backslash:
      return "path contains a backslash ('/' is the only separator)";
    case path_problem::empty_component:
      return "path has an empty component (a '//' run or a trailing '/')";
    case path_problem::dot_component:
      return "path has a '.' component";
    case path_problem::dotdot_component:
      return "path has a '..' component, which escapes the extraction root";
  }
  return "unknown problem";
}

[[nodiscard]] constexpr auto check_archive_path(std::string_view p) noexcept
    -> path_problem
{
  if (p.empty())
    return path_problem::empty;
  if (static_cast<int64_t>(p.size()) > max_archive_path_size)
    return path_problem::too_long;

  // A single leading '/' marks an absolute-looking key and is not treated as
  // an empty first component.
  std::size_t i = (p.front() == '/') ? 1u : 0u;
  if (i == p.size())
    return path_problem::empty_component; // the path was just "/"

  std::size_t component_start = i;
  for (; i <= p.size(); i++)
  {
    const bool at_end = (i == p.size());
    const char c = at_end ? '/' : p[i];

    if (!at_end)
    {
      if (c == '\0')
        return path_problem::contains_nul;
      if (c == '\\')
        return path_problem::contains_backslash;
    }

    if (c != '/')
      continue;

    const auto component = p.substr(component_start, i - component_start);
    if (component.empty())
      return path_problem::empty_component;
    if (component == ".")
      return path_problem::dot_component;
    if (component == "..")
      return path_problem::dotdot_component;
    component_start = i + 1;
  }
  return path_problem::ok;
}

//! True when `p` is safe to join onto an extraction directory (after stripping
//! any leading separators).
[[nodiscard]] constexpr auto is_safe_archive_path(std::string_view p) noexcept -> bool
{
  return check_archive_path(p) == path_problem::ok;
}

} // namespace uvfs
